// |jit-test| skip-if: !wasmComponentsEnabled()

// Every scalar type round trips through a lifted export and a lowered import.
// The JS-API behavior is covered by the WPT tests in wasm/component/jsapi.

const flatTypes = {
  bool: "i32", s8: "i32", u8: "i32", s16: "i32", u16: "i32", s32: "i32",
  u32: "i32", s64: "i64", u64: "i64", f32: "f32", f64: "f64", char: "i32",
};

function roundtrip(type, echo) {
  const flat = flatTypes[type];
  return new WebAssembly.ComponentInstance(new WebAssembly.Component(wasmTextToBinary(`(component
    (import "echo" (func $echo (param "x" ${type}) (result ${type})))
    (core func $echo' (canon lower (func $echo)))
    (core module $m
      (import "" "echo" (func $echo (param ${flat}) (result ${flat})))
      (func (export "roundtrip") (param ${flat}) (result ${flat})
        (call $echo (local.get 0))))
    (core instance $i (instantiate $m (with "" (instance (export "echo" (func $echo'))))))
    (func $roundtrip (param "x" ${type}) (result ${type})
      (canon lift (core func $i "roundtrip")))
    (export "roundtrip" (func $roundtrip))
  )`)), { echo }).exports.roundtrip;
}

for (const [type, values] of Object.entries({
  bool: [true, false],
  s8: [-128, 127], u8: [0, 255], s16: [-32768, 32767], u16: [0, 65535],
  s32: [-(2 ** 31), 2 ** 31 - 1], u32: [0, 2 ** 32 - 1],
  s64: [-(2n ** 63n), 2n ** 63n - 1n], u64: [0n, 2n ** 64n - 1n],
  f32: [1.5, -0, Infinity, NaN], f64: [0.1, -0, -Infinity, NaN],
  char: ["a", "\u{10ffff}"],
})) {
  let seen;
  const f = roundtrip(type, x => (seen = x));
  for (const value of values) {
    assertEq(f(value), value);
    assertEq(seen, value);
  }
}

// f32 values are rounded, integers wrap, and 64-bit integers don't accept
// Numbers.
assertEq(roundtrip("f32", x => x)(0.1), Math.fround(0.1));
assertEq(roundtrip("s8", x => x)(128), -128);
assertEq(roundtrip("u32", x => x)(-1), 2 ** 32 - 1);
assertEq(roundtrip("u64", x => x)(-1n), 2n ** 64n - 1n);
assertEq(roundtrip("s64", x => x)(2n ** 63n), -(2n ** 63n));
assertErrorMessage(() => roundtrip("s64", x => x)(-5), TypeError, /BigInt/);

// A lifted char must be a Unicode scalar value.
{
  const f = new WebAssembly.ComponentInstance(new WebAssembly.Component(wasmTextToBinary(`(component
    (core module $m (func (export "f") (result i32) i32.const 0xd800))
    (core instance $i (instantiate $m))
    (func $f (result char) (canon lift (core func $i "f")))
    (export "f" (func $f))
  )`))).exports.f;
  assertErrorMessage(() => f(), WebAssembly.RuntimeError, /bad codepoint/);
}
