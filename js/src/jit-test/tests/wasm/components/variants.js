// |jit-test| skip-if: !wasmComponentsEnabled()

// Variants in component adapters. The JS-API behavior is covered by the WPT
// tests in wasm/component/jsapi.

// A flattened variant whose cases have different flat types, which are joined
// to i32 i64. `roundtrip` passes it through the import `echo`.
const { roundtrip } = new WebAssembly.ComponentInstance(
  new WebAssembly.Component(wasmTextToBinary(`(component
    (type $v (variant (case "none") (case "f" f32) (case "d" f64) (case "l" u64)
                      (case "pair" (tuple f32 u32))))
    (import "v" (type $v' (eq $v)))
    (import "echo" (func $echo (param "x" $v') (result u32)))
    (core func $echo' (canon lower (func $echo)))
    (core module $m
      (import "" "echo" (func $echo (param i32 i64 i32) (result i32)))
      (func (export "roundtrip") (param i32 i64 i32) (result i32)
        (call $echo (local.get 0) (local.get 1) (local.get 2))))
    (core instance $i (instantiate $m (with "" (instance (export "echo" (func $echo'))))))
    (func $roundtrip (param "x" $v') (result u32) (canon lift (core func $i "roundtrip")))
    (export "roundtrip" (func $roundtrip))
  )`)), { echo: x => { seen = x; return 0; } }).exports;
let seen;
for (const value of [
  { kind: "none" },
  { kind: "f", value: 1.5 },
  { kind: "f", value: NaN },
  { kind: "d", value: -0.1 },
  { kind: "l", value: 2n ** 64n - 1n },
  { kind: "pair", value: [-2.5, 0xffffffff] },
]) {
  roundtrip(value);
  assertEq(JSON.stringify(seen, (k, v) => typeof v === "bigint" ? `${v}n` : v),
           JSON.stringify(value, (k, v) => typeof v === "bigint" ? `${v}n` : v));
  assertEq(Object.getPrototypeOf(seen), null);
}
assertErrorMessage(() => roundtrip({ kind: "None" }), TypeError, /'None' is not a case/);
assertErrorMessage(() => roundtrip(null), TypeError, /not a non-null object/);

// Lifting a discriminant that is not a case traps.
{
  const f = new WebAssembly.ComponentInstance(new WebAssembly.Component(wasmTextToBinary(`(component
    (core module $m (func (export "f") (result i32) i32.const 3))
    (core instance $i (instantiate $m))
    (type $e (enum "a" "b" "c"))
    (export $e' "e" (type $e))
    (func $f (result $e') (canon lift (core func $i "f")))
    (export "f" (func $f))
  )`))).exports.f;
  assertErrorMessage(f, WebAssembly.RuntimeError, /invalid variant discriminant/);
}

// A lifted function that returns a result error throws a ComponentError with
// the payload as its data. A host function returning a result that throws
// returns the error case with the thrown value as its payload.
{
  const component = new WebAssembly.Component(wasmTextToBinary(`(component
    (import "check" (func $check (param "x" u32) (result (result u32 (error string)))))
    (core module $mem
      (memory (export "mem") 1)
      (global $bump (mut i32) (i32.const 16))
      (func (export "realloc") (param i32 i32 i32) (param $size i32) (result i32)
        (local $ptr i32)
        (local.set $ptr (global.get $bump))
        (global.set $bump (i32.add (global.get $bump) (i32.and (i32.add (local.get $size) (i32.const 3)) (i32.const -4))))
        (local.get $ptr)))
    (core instance $mem (instantiate $mem))
    (core func $check' (canon lower (func $check) (memory (core memory $mem "mem"))
                       (realloc (core func $mem "realloc"))))
    (core module $m
      (import "" "check" (func $check (param i32 i32)))
      (func (export "run") (param i32) (result i32)
        (call $check (local.get 0) (i32.const 0x100))
        (i32.const 0x100)))
    (core instance $i (instantiate $m (with "" (instance (export "check" (func $check'))))))
    (func $run (param "x" u32) (result (result u32 (error string)))
      (canon lift (core func $i "run") (memory (core memory $mem "mem"))))
    (export "run" (func $run))
  )`));
  const { run } = new WebAssembly.ComponentInstance(component, {
    check(x) {
      if (x === 0) {
        throw "zero";
      }
      if (x === 1) {
        throw { toString() { throw new Error("can't convert"); } };
      }
      if (x === 2) {
        throw new WebAssembly.Exception(new WebAssembly.Tag({ parameters: [] }), []);
      }
      return x * 2;
    },
  }).exports;
  assertEq(run(4), 8);
  try {
    run(0);
    assertEq(true, false);
  } catch (e) {
    assertEq(e instanceof WebAssembly.ComponentError, true);
    assertEq(e.data, "zero");
  }
  // A wasm exception is caught like any other.
  try {
    run(2);
    assertEq(true, false);
  } catch (e) {
    assertEq(e.data, "[object WebAssembly.Exception]");
  }
  // A thrown value that can't be converted to the error type traps.
  assertErrorMessage(() => run(1), WebAssembly.RuntimeError,
                     /exception thrown by a component function import/);
}
