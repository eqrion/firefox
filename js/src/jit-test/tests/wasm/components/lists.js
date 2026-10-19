// |jit-test| skip-if: !wasmComponentsEnabled()

// Lists in component adapters. The JS-API behavior is covered by the WPT tests
// in wasm/component/jsapi.

// `take(l: list<list<u16>>)` records the sizes that realloc was called with,
// which `sizes` returns.
const { take, sizes } = new WebAssembly.ComponentInstance(
  new WebAssembly.Component(wasmTextToBinary(`(component
    (core module $m
      (memory (export "mem") 1)
      (global $bump (mut i32) (i32.const 1024))
      (global $count (mut i32) (i32.const 0))
      (func (export "realloc") (param $old i32) (param $oldSize i32)
                               (param $align i32) (param $newSize i32) (result i32)
        (local $ptr i32)
        (i32.store (i32.add (i32.const 8) (i32.shl (global.get $count) (i32.const 2)))
                   (local.get $newSize))
        (global.set $count (i32.add (global.get $count) (i32.const 1)))
        (local.set $ptr
          (i32.and (i32.add (global.get $bump) (i32.sub (local.get $align) (i32.const 1)))
                   (i32.sub (i32.const 0) (local.get $align))))
        (global.set $bump (i32.add (local.get $ptr) (local.get $newSize)))
        (local.get $ptr))
      (func (export "take") (param i32 i32))
      (func (export "sizes") (result i32)
        (i32.store (i32.const 0) (i32.const 8))
        (i32.store (i32.const 4) (global.get $count))
        (global.set $count (i32.const 0))
        (i32.const 0)))
    (core instance $i (instantiate $m))
    (func $take (param "l" (list (list u16)))
      (canon lift (core func $i "take") (memory (core memory $i "mem"))
                  (realloc (core func $i "realloc"))))
    (export "take" (func $take))
    (func $sizes (result (list u32))
      (canon lift (core func $i "sizes") (memory (core memory $i "mem"))))
    (export "sizes" (func $sizes))
  )`))).exports;

// Each list is allocated once, at its final size, and the outer list before
// its elements. Empty lists are allocated too.
take([[1, 2, 3], [], [4]]);
assertDeepEq(sizes(), new Uint32Array([3 * 8, 3 * 2, 0, 1 * 2]));
take([]);
assertDeepEq(sizes(), new Uint32Array([0]));

// Lifting a list that is out of bounds or misaligned traps.
function liftList(ptr, len) {
  return new WebAssembly.ComponentInstance(new WebAssembly.Component(wasmTextToBinary(`(component
    (core module $m
      (memory (export "mem") 1)
      (func (export "f") (result i32)
        (i32.store (i32.const 0) (i32.const ${ptr}))
        (i32.store (i32.const 4) (i32.const ${len}))
        (i32.const 0)))
    (core instance $i (instantiate $m))
    (func $f (result (list u32)) (canon lift (core func $i "f") (memory (core memory $i "mem"))))
    (export "f" (func $f))
  )`))).exports.f;
}
assertDeepEq(liftList(8, 0)(), new Uint32Array());
assertErrorMessage(liftList(2, 1), WebAssembly.RuntimeError, /unaligned memory access/);
assertErrorMessage(liftList(0xfffc, 2), WebAssembly.RuntimeError, /index out of bounds/);
assertErrorMessage(liftList(8, 0x40000000), WebAssembly.RuntimeError, /index out of bounds/);
