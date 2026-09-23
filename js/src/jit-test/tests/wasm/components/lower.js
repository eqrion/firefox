// |jit-test| skip-if: !wasmComponentsEnabled()

// Lowered function imports. The JS-API behavior is mostly covered by the WPT
// tests in wasm/component/jsapi.

// Calls the import `f` with the arguments of the export `call`.
const bytes = wasmTextToBinary(`(component
  (import "f" (func $f (param "a" u32) (param "b" s8) (result u32)))
  (core func $f' (canon lower (func $f)))
  (core module $m
    (import "" "f" (func $f (param i32 i32) (result i32)))
    (func (export "call") (param i32 i32) (result i32)
      (call $f (local.get 0) (local.get 1))))
  (core instance $i (instantiate $m (with "" (instance (export "f" (func $f'))))))
  (func $call (param "a" u32) (param "b" s8) (result u32)
    (canon lift (core func $i "call")))
  (export "call" (func $call))
)`);
const component = new WebAssembly.Component(bytes);
function instantiate(f) {
  return new WebAssembly.ComponentInstance(component, { f }).exports.call;
}

// The import is called with an undefined receiver and exactly one argument per
// parameter, converted with ToJSValue.
{
  let receiver, args;
  const call = instantiate(function () {
    "use strict";
    receiver = this;
    args = [...arguments];
    return 5;
  });
  assertEq(call(0xffffffff, -1, "extra"), 5);
  assertEq(receiver, undefined);
  assertDeepEq(args, [0xffffffff, -1]);
}

// A wasm function import is called like any other function, so its own
// signature does not matter.
{
  const { f } = wasmEvalText(`(module
    (func (export "f") (param f64) (result f64) (f64.add (local.get 0) (f64.const 0.5))))`).exports;
  assertEq(instantiate(f)(1, 0), 1);
}

// Exceptions from the import, and results that fail ToComponentValue, trap.
for (const f of [
  () => { throw new Error("host"); },
  () => 1n,
  () => ({ valueOf() { throw new Error("valueOf"); } }),
]) {
  const call = instantiate(f);
  assertErrorMessage(() => call(0, 0), WebAssembly.RuntimeError,
                     /exception thrown by a component function import/);
  assertErrorMessage(() => call(0, 0), WebAssembly.RuntimeError, /locked down/);
}

// A lowered function has one identity, however many times it is used.
{
  const i = new WebAssembly.ComponentInstance(new WebAssembly.Component(wasmTextToBinary(`(component
    (import "f" (func $f))
    (core func $f' (canon lower (func $f)))
    (core instance $a (export "f" (func $f')))
    (core instance $b (export "f" (func $f')))
  )`)), { f() {} });
  assertEq(wasmComponentCoreInstanceExports(i, 0).f,
           wasmComponentCoreInstanceExports(i, 1).f);
}
