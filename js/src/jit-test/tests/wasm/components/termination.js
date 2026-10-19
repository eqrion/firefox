// |jit-test| skip-if: !wasmComponentsEnabled(); exitstatus: 6

// Uncatchable termination while converting a lowered import's result stops
// the adapter, without continuing with the result.
const bytes = wasmTextToBinary(`(component
  (import "f" (func $f (result u32)))
  (core func $f' (canon lower (func $f)))
  (core module $m
    (import "" "f" (func $f (result i32)))
    (func (export "call") (result i32) (call $f)))
  (core instance $i (instantiate $m (with "" (instance (export "f" (func $f'))))))
  (func $call (result u32) (canon lift (core func $i "call")))
  (export "call" (func $call))
)`);
const { call } = new WebAssembly.ComponentInstance(
  new WebAssembly.Component(bytes),
  {
    f() {
      return {
        valueOf() {
          interruptIf(true);
          for (;;) {}
        },
      };
    },
  },
).exports;

setInterruptCallback(() => false);
call();
quit(0);
