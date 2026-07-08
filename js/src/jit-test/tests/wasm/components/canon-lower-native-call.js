// |jit-test| skip-if: !wasmComponentsEnabled(); local-include:canon-instantiate-harness.js

// TryCallWebIDLNative's fast path used to be gated behind a debug-only
// MOZ_CRASH right after overload selection, so no real `canon lower` call
// had ever actually reached a native entry - this exercises that it now
// does, for a callee whose result spills to a caller-supplied out-pointer
// (a component `own`/`string`/etc. flat form wider than MaxFlatResults),
// proving the native path threads that pointer through instead of always
// writing at offset 0.

// n -> "value:<n>" (see webidlTestNativeStringResult's C++ side). A DOMString
// result always spills (its flat form is (ptr,len), 2 slots > MaxFlatResults),
// so the lowered import's core signature drops its declared result entirely
// and gains a trailing out-pointer param; the caller reads the (ptr,len)
// header back out of its own choice of scratch address.
// canon lower's memory/realloc bind to $libc's memory (see lowerThrough), not
// any memory $caller might declare for itself - so $caller just returns the
// header address it chose and JS reads the (ptr,len) pair straight out of
// the returned `memory`, rather than the wasm side re-loading from a memory
// instance of its own that the write never touched.
const { caller, memory } = lowerThrough(
  `(param "n" s32) (result string)`,
  webidlTestNativeStringResult,
  `(import "env" "f" (func $f (param i32 i32)))
   (func (export "call") (param $n i32) (result i32)
     (call $f (local.get $n) (i32.const 50000))
     (i32.const 50000))`
);

function readUtf8(ptr, len) {
  const bytes = new Uint8Array(memory.buffer, ptr, len);
  return String.fromCharCode(...bytes);
}

const headerPtr = caller.call(7);
const view = new DataView(memory.buffer);
const strPtr = view.getUint32(headerPtr, true);
const len = view.getUint32(headerPtr + 4, true);
assertEq(readUtf8(strPtr, len), "value:7");
