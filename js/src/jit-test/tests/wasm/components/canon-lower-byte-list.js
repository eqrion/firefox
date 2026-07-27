// |jit-test| skip-if: !wasmComponentsEnabled(); local-include:canon-instantiate-harness.js

// A component `list<u8>` bridges to/from a byte-typed WebIDL buffer source
// (Uint8Array / Uint8ClampedArray / ArrayBuffer) as a single bulk copy. That
// crosses the WebIDL overload class boundary in neither direction - Sequence
// and BufferSource are both Object - so what makes it work is the byte-element
// check in BuildCoerceStep plus the memcpy primitives ReadCoercedByteListRange
// and WriteCoercedByteListFromBufferSource.
//
// The webidlTestNativeByteArray* fixtures have native overload entries; the
// webidlTestByteArray*Dynamic ones advertise the identical schema with none,
// forcing the fallback.
//
// Note what the fallback can and cannot reach. `canon lower`'s dynamic path
// always builds a variadic-`any` operation type (see WasmComponentLoweredJS),
// discarding the callee's real WebIDL schema, so a declared buffer-source
// *param* never produces a StepKind::BufferSource there - it comes through as
// StepKind::Any, i.e. a plain JS Array. Only the native entry path sees the
// real param type. The result direction does still work in the fallback,
// because a declared `any` return makes WalkResultStepFromJS convert against
// the component's own canonical type (sequence<octet>), and JSToWebIDLSequence
// iterates a typed array fine - just element-wise instead of as one memcpy.

// ---------------------------------------------------------------------------
// Param direction: component list<u8> -> Uint8ClampedArray
// ---------------------------------------------------------------------------

// A `list<u8>` param flattens to two i32s (ptr, len). canon lower's memory
// binds to $libc's (see lowerThrough), so the bytes are written into that
// memory from JS and the caller just forwards the address it was given.
function sumThrough(callee) {
  const { caller, memory } = lowerThrough(
    `(param "a" (list u8)) (result s32)`,
    callee,
    `(import "env" "f" (func $f (param i32 i32) (result i32)))
     (func (export "call") (param $ptr i32) (param $len i32) (result i32)
       (call $f (local.get $ptr) (local.get $len)))`
  );
  return (bytes) => {
    const ptr = 50000;
    new Uint8Array(memory.buffer, ptr, bytes.length).set(bytes);
    return caller.call(ptr, bytes.length);
  };
}

// Native path only - see the note above for why the fallback can't get a
// buffer-source param.
{
  const sum = sumThrough(webidlTestNativeByteArraySum);
  assertEq(sum([]), 0);
  assertEq(sum([1, 2, 3]), 6);
  assertEq(sum([255, 255]), 510);
  // 300 elements: past any plausible small-buffer special case, and enough
  // that a per-element loop would differ observably from a memcpy if the
  // stride were wrong.
  const big = Array.from({ length: 300 }, (_, i) => i % 256);
  assertEq(sum(big), big.reduce((a, b) => a + b, 0));
}

// The fallback hands a plain JS Array to a callee whose declared param is a
// buffer source, so the fixture rejects it. Asserted rather than skipped so
// this test notices if the fallback ever starts threading the real schema.
{
  const sum = sumThrough(webidlTestByteArraySumDynamic);
  assertErrorMessage(() => sum([1, 2, 3]), Error, /expected a typed array/);
}

// ---------------------------------------------------------------------------
// Result direction: Uint8ClampedArray -> component list<u8>
// ---------------------------------------------------------------------------

// A `list<u8>` result flattens to (ptr,len), 2 slots > MaxFlatResults, so it
// spills to a caller-supplied out-pointer: the lowered import drops its
// declared result and gains a trailing out-param. The caller returns the
// header address it chose so JS can read the pair straight out of `memory`.
function resultThrough(callee) {
  const { caller, memory } = lowerThrough(
    `(param "n" s32) (result (list u8))`,
    callee,
    `(import "env" "f" (func $f (param i32 i32)))
     (func (export "call") (param $n i32) (result i32)
       (call $f (local.get $n) (i32.const 50000))
       (i32.const 50000))`
  );
  return (n) => {
    const headerPtr = caller.call(n);
    const view = new DataView(memory.buffer);
    const ptr = view.getUint32(headerPtr, true);
    const len = view.getUint32(headerPtr + 4, true);
    return Array.from(new Uint8Array(memory.buffer, ptr, len));
  };
}

for (const callee of [webidlTestNativeByteArrayResult,
                      webidlTestByteArrayResultDynamic]) {
  const result = resultThrough(callee);
  assertEq(JSON.stringify(result(0)), "[]");
  assertEq(JSON.stringify(result(4)), "[0,1,2,3]");
  const big = result(300);
  assertEq(big.length, 300);
  assertEq(big.every((b, i) => b === i % 256), true);
}

// ---------------------------------------------------------------------------
// Only `list<u8>` bridges to a byte view
// ---------------------------------------------------------------------------

// BuildCoerceStep's byte-element check rejects a wider (or differently signed,
// or float) element type, and its source-kind check rejects a non-list -
// `string` is worth covering because it also lands in the Object overload
// class, so nothing but that check stops it being misread as a (ptr,len) list.
//
// A rejected shape makes the step a Trap, which disqualifies the *native* path
// (SelectNativeOverload treats a doomed adapter as ineligible). It does not
// surface as a trap at the call, though: the variadic-`any` fallback then
// happily lowers the value as a plain JS Array (or string), which the fixture
// rejects because its declared param is a buffer source. So that error is the
// observable proof the byte path was not taken - contrast the `list<u8>` cases
// above, which return a real sum.
for (const paramType of ["(list u32)", "(list s8)", "(list f32)", "string"]) {
  const { caller } = lowerThrough(
    `(param "a" ${paramType}) (result s32)`,
    webidlTestNativeByteArraySum,
    `(import "env" "f" (func $f (param i32 i32) (result i32)))
     (func (export "call") (param $ptr i32) (param $len i32) (result i32)
       (call $f (local.get $ptr) (local.get $len)))`
  );
  assertErrorMessage(() => caller.call(50000, 2), Error,
                     /expected a typed array/);
}

// ---------------------------------------------------------------------------
// An out-of-bounds (ptr,len) header traps rather than reading wild memory
// ---------------------------------------------------------------------------

{
  const { caller, memory } = lowerThrough(
    `(param "a" (list u8)) (result s32)`,
    webidlTestNativeByteArraySum,
    `(import "env" "f" (func $f (param i32 i32) (result i32)))
     (func (export "call") (param $ptr i32) (param $len i32) (result i32)
       (call $f (local.get $ptr) (local.get $len)))`
  );
  const byteLength = memory.buffer.byteLength;
  assertErrorMessage(() => caller.call(byteLength - 4, 64), Error,
                     /out of bounds/);
  // Length alone overflowing the end is caught too, not just a wild pointer.
  assertErrorMessage(() => caller.call(0, byteLength + 1), Error,
                     /out of bounds/);
}
