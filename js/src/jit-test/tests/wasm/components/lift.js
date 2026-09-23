// |jit-test| skip-if: !wasmComponentsEnabled()

// Lifted functions exported to JS. The JS-API behavior is mostly covered by the
// WPT tests in wasm/component/jsapi.

function instantiate(text) {
  return new WebAssembly.ComponentInstance(
    new WebAssembly.Component(wasmTextToBinary(text))).exports;
}

// Each integer type accepts exactly its range, after truncation toward zero.
for (const [type, min, max] of [
  ["s8", -128, 127],
  ["u8", 0, 255],
  ["s16", -32768, 32767],
  ["u16", 0, 65535],
  ["s32", -(2 ** 31), 2 ** 31 - 1],
  ["u32", 0, 2 ** 32 - 1],
]) {
  const { id } = instantiate(`(component
    (core module $m (func (export "id") (param i32) (result i32) local.get 0))
    (core instance $i (instantiate $m))
    (func $id (param "x" ${type}) (result ${type})
      (canon lift (core func $i "id")))
    (export "id" (func $id))
  )`);
  assertEq(id(min), min);
  assertEq(id(max), max);
  assertEq(id(min - 0.5), min);
  assertEq(id(max + 0.5), max);
  assertEq(id(min - 1), max);
  assertEq(id(max + 1), min);
}

// Lifting a result keeps only the bits of its type.
for (const [type, expected] of [
  ["s8", -1],
  ["u8", 255],
  ["s16", -1],
  ["u16", 65535],
  ["s32", -1],
  ["u32", 2 ** 32 - 1],
]) {
  const { f } = instantiate(`(component
    (core module $m (func (export "f") (result i32) i32.const -1))
    (core instance $i (instantiate $m))
    (func $f (result ${type}) (canon lift (core func $i "f")))
    (export "f" (func $f))
  )`);
  assertEq(f(), expected);
}
{
  const { f } = instantiate(`(component
    (core module $m (func (export "f") (result i32) i32.const 0x1ff))
    (core instance $i (instantiate $m))
    (func $f (result u8) (canon lift (core func $i "f")))
    (export "f" (func $f))
  )`);
  assertEq(f(), 0xff);
}

// A wasm exception thrown out of a lifted function traps, and locks down only
// the instance it came from.
{
  const text = `(component
    (core module $m
      (tag $t)
      (func (export "throw") throw $t)
      (func (export "ok") (result i32) i32.const 1))
    (core instance $i (instantiate $m))
    (func $throw (canon lift (core func $i "throw")))
    (func $ok (result u32) (canon lift (core func $i "ok")))
    (export "throw" (func $throw))
    (export "ok" (func $ok))
  )`;
  const a = instantiate(text);
  const b = instantiate(text);
  assertErrorMessage(() => a.throw(), WebAssembly.RuntimeError,
                     /exception thrown out of a lifted component function/);
  assertErrorMessage(() => a.ok(), WebAssembly.RuntimeError, /locked down/);
  assertEq(b.ok(), 1);
}

// A conversion error before calling into the guest does not lock down the
// instance.
{
  const { id } = instantiate(`(component
    (core module $m (func (export "id") (param i32) (result i32) local.get 0))
    (core instance $i (instantiate $m))
    (func $id (param "x" u32) (result u32) (canon lift (core func $i "id")))
    (export "id" (func $id))
  )`);
  assertErrorMessage(() => id(1n), TypeError, /BigInt/);
  assertEq(id(1), 1);
}

// Exporting an exported function creates a new JS function for the same
// lifted function.
{
  const { a, b } = instantiate(`(component
    (core module $m (func (export "f") (result i32) i32.const 7))
    (core instance $i (instantiate $m))
    (func $f (result u32) (canon lift (core func $i "f")))
    (export $a "a" (func $f))
    (export "b" (func $a))
  )`);
  assertEq(a(), 7);
  assertEq(b(), 7);
  assertEq(a === b, false);
  assertEq(b.name, "b");
}

// Unsupported types fail at instantiation.
assertErrorMessage(() => instantiate(`(component
  (type $r (resource (rep i32)))
  (core module $m (func (export "f") (result i32) i32.const 0))
  (core instance $i (instantiate $m))
  (func $f (result (own $r)) (canon lift (core func $i "f")))
  (export "f" (func $f))
)`), Error, /component function type is not supported yet/);
