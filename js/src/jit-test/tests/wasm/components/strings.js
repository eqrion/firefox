// |jit-test| skip-if: !wasmComponentsEnabled()

// Strings in component adapters. The JS-API behavior is covered by the WPT
// tests in wasm/component/jsapi.

// `take(s: string, n: u32) -> string` returns `s`, and `count` returns the
// number of sizes logged by realloc, two per call.
const bytes = wasmTextToBinary(`(component
  (core module $m
    (memory (export "mem") 1)
    (global $bump (mut i32) (i32.const 16))
    (global $log (mut i32) (i32.const 0x8000))
    (func $log (param i32)
      (i32.store (global.get $log) (local.get 0))
      (global.set $log (i32.add (global.get $log) (i32.const 4))))
    (func (export "realloc") (param $old i32) (param $oldSize i32)
                             (param $align i32) (param $newSize i32) (result i32)
      (local $ptr i32)
      (call $log (local.get $oldSize))
      (call $log (local.get $newSize))
      (local.set $ptr (global.get $bump))
      (global.set $bump (i32.add (global.get $bump) (local.get $newSize)))
      (memory.copy (local.get $ptr) (local.get $old) (local.get $oldSize))
      (local.get $ptr))
    (func (export "take") (param i32 i32 i32) (result i32)
      (i32.store (i32.const 0) (local.get 0))
      (i32.store (i32.const 4) (local.get 1))
      (i32.const 0))
    (func (export "count") (result i32)
      (i32.div_u (i32.sub (global.get $log) (i32.const 0x8000)) (i32.const 4))))
  (core instance $i (instantiate $m))
  (func $take (param "s" string) (param "n" u32) (result string)
    (canon lift (core func $i "take") (memory (core memory $i "mem"))
                (realloc (core func $i "realloc"))))
  (export "take" (func $take))
  (func $count (result u32) (canon lift (core func $i "count")))
  (export "count" (func $count))
)`);

function instantiate() {
  const { take, count } = new WebAssembly.ComponentInstance(
    new WebAssembly.Component(bytes)).exports;
  return { take, count };
}

// A UTF-8 string is allocated at one byte per code unit, and only grown to the
// worst case and shrunk if it is not ASCII.
{
  const { take, count } = instantiate();
  assertEq(take("hello", 0), "hello");
  assertEq(count(), 2, "one realloc for ASCII");
  assertEq(take("héllo", 0), "héllo");
  assertEq(count(), 2 * 4, "three more reallocs for non-ASCII");
  assertEq(take("", 0), "");
  assertEq(count(), 2 * 5, "empty strings are allocated too");
}

// A latin1+utf16 string is allocated as Latin-1, and only grown to UTF-16 if
// it isn't Latin-1, in which case its length is tagged.
{
  const { take, count, lastLength } = new WebAssembly.ComponentInstance(
    new WebAssembly.Component(wasmTextToBinary(`(component
      (core module $m
        (memory (export "mem") 1)
        (global $bump (mut i32) (i32.const 16))
        (global $count (mut i32) (i32.const 0))
        (global $length (mut i32) (i32.const 0))
        (func (export "realloc") (param $old i32) (param $oldSize i32)
                                 (param $align i32) (param $newSize i32) (result i32)
          (local $ptr i32)
          (global.set $count (i32.add (global.get $count) (i32.const 1)))
          (local.set $ptr
            (i32.and (i32.add (global.get $bump) (i32.sub (local.get $align) (i32.const 1)))
                     (i32.sub (i32.const 0) (local.get $align))))
          (global.set $bump (i32.add (local.get $ptr) (local.get $newSize)))
          (memory.copy (local.get $ptr) (local.get $old) (local.get $oldSize))
          (local.get $ptr))
        (func (export "take") (param i32 i32) (result i32)
          (global.set $length (local.get 1))
          (i32.store (i32.const 0) (local.get 0))
          (i32.store (i32.const 4) (local.get 1))
          (i32.const 0))
        (func (export "count") (result i32) (global.get $count))
        (func (export "last-length") (result i32) (global.get $length)))
      (core instance $i (instantiate $m))
      (func $take (param "s" string) (result string)
        (canon lift (core func $i "take") (memory (core memory $i "mem"))
                    (realloc (core func $i "realloc")) string-encoding=latin1+utf16))
      (export "take" (func $take))
      (func $count (result u32) (canon lift (core func $i "count")))
      (export "count" (func $count))
      (func $last-length (result u32) (canon lift (core func $i "last-length")))
      (export "last-length" (func $last-length))
    )`))).exports;
  assertEq(take("héllo ÿ"), "héllo ÿ");
  assertEq(count(), 1);
  assertEq(lastLength(), 7);
  assertEq(take("héllo Ā"), "héllo Ā");
  assertEq(count(), 3);
  assertEq(lastLength(), (7 | 0x80000000) >>> 0);
  assertEq(take("a\ud800"), "a�");
}

// Lone surrogates become U+FFFD.
{
  const { take } = instantiate();
  assertEq(take("a\ud800b", 0), "a�b");
  assertEq(take("\udc00", 0), "�");
}

// A conversion failure after realloc has run locks the instance down, and the
// original exception still propagates. Before realloc runs, it doesn't.
{
  const { take } = instantiate();
  const error = new Error("valueOf");
  assertThrowsValue(() => take({ toString() { throw error; } }, 0), error);
  assertEq(take("still works", 0), "still works");
  assertThrowsValue(() => take("a", { valueOf() { throw error; } }), error);
  assertErrorMessage(() => take("a", 0), WebAssembly.RuntimeError, /locked down/);
}

// Lifting a string that is not valid in its encoding traps.
function liftBytes(bytes) {
  return new WebAssembly.ComponentInstance(new WebAssembly.Component(wasmTextToBinary(`(component
    (core module $m
      (memory (export "mem") 1)
      (data (i32.const 8) "${bytes}")
      (func (export "f") (result i32)
        (i32.store (i32.const 0) (i32.const 8))
        (i32.store (i32.const 4) (i32.const ${bytes.split("\\").length - 1}))
        (i32.const 0)))
    (core instance $i (instantiate $m))
    (func $f (result string) (canon lift (core func $i "f") (memory (core memory $i "mem"))))
    (export "f" (func $f))
  )`))).exports.f;
}
assertEq(liftBytes("\\68\\c3\\a9")(), "hé");
assertErrorMessage(liftBytes("\\c3"), WebAssembly.RuntimeError, /invalid string encoding/);
assertErrorMessage(liftBytes("\\ed\\a0\\80"), WebAssembly.RuntimeError, /invalid string encoding/);

// Lifting a string or result pointer that is out of bounds or misaligned
// traps.
for (const [ptr, len, message] of [
  [0xfffff, 2, /index out of bounds/],
  [0x10000, 1, /index out of bounds/],
  [0, 0x10000000, /string is too long/],
]) {
  const f = new WebAssembly.ComponentInstance(new WebAssembly.Component(wasmTextToBinary(`(component
    (core module $m
      (memory (export "mem") 1)
      (func (export "f") (result i32)
        (i32.store (i32.const 0) (i32.const ${ptr}))
        (i32.store (i32.const 4) (i32.const ${len}))
        (i32.const 0)))
    (core instance $i (instantiate $m))
    (func $f (result string) (canon lift (core func $i "f") (memory (core memory $i "mem"))))
    (export "f" (func $f))
  )`))).exports.f;
  assertErrorMessage(f, WebAssembly.RuntimeError, message);
}
{
  const f = new WebAssembly.ComponentInstance(new WebAssembly.Component(wasmTextToBinary(`(component
    (core module $m
      (memory (export "mem") 1)
      (func (export "f") (result i32) (i32.const 2)))
    (core instance $i (instantiate $m))
    (func $f (result string) (canon lift (core func $i "f") (memory (core memory $i "mem"))))
    (export "f" (func $f))
  )`))).exports.f;
  assertErrorMessage(f, WebAssembly.RuntimeError, /unaligned memory access/);
}
