// |jit-test| skip-if: !wasmComponentsEnabled(); test-also=--wasm-compiler=optimizing

// resource.drop's Ion-inlined fast path (FunctionCompiler::
// callComponentResourceDrop in WasmIonCompile.cpp) only activates for a
// Native-repr handle - which requires the imported resource type to be
// bound to a WebIDLInterface with native ops, and the value being lifted to
// be a real WebIDLNativeResource (see canon-resource-native-repr.js for the
// classification rules). This exercises that fast path end to end: bounds,
// present+native, and lend checks all pass in JIT code, the free-list push
// happens inline, and only WebIDLInterface::releaseNative_ crosses into C++
// - and confirms it agrees with the always-C++ builtin (Instance::
// componentResourceDrop) for every case that falls back to it: index 0, an
// out-of-bounds index, a double drop, a Wrapped value, and a table grown
// past its initial capacity.
//
// `test-also=--wasm-compiler=optimizing` runs this file twice: once under
// the default tiering and once with Ion forced from the start, so both the
// new inline fast path and the unchanged Baseline/slow-path route
// (Instance::componentResourceDrop is the oracle either way) get exercised.

const NativeIface = new WebIDLInterface("NativeIface", null, /* withNativeOps */ true);

function makeInstance() {
  const component = new WebAssembly.Component(wasmTextToBinary(`
    (component
      (import "r" (type $T (sub resource)))

      (core func $T_drop (canon resource.drop $T))
      (core instance $env (export "drop" (func $T_drop)))

      (core module $main
        (import "env" "drop" (func $drop (param i32)))
        (func (export "consume") (param $h i32) (result i32) local.get $h)
        (func (export "drop-raw") (param $h i32) (call $drop (local.get $h)))
      )
      (core instance $mainInst (instantiate $main (with "env" (instance $env))))
      (alias core export $mainInst "consume" (core func $consumeCore))
      (alias core export $mainInst "drop-raw" (core func $dropRawCore))

      (type $consumeOwnType (func (param "h" (own $T)) (result s32)))
      (func $consumeOwn (type $consumeOwnType) (canon lift (core func $consumeCore)))
      (export "consume" (func $consumeOwn))

      (type $consumeBorrowType (func (param "h" (borrow $T)) (result s32)))
      (func $consumeBorrow (type $consumeBorrowType) (canon lift (core func $consumeCore)))
      (export "consume-borrow" (func $consumeBorrow))

      (type $dropRawType (func (param "h" s32)))
      (func $dropRawFn (type $dropRawType) (canon lift (core func $dropRawCore)))
      (export "drop-raw" (func $dropRawFn))
    )
  `));
  return new WebAssembly.ComponentInstance(component, { R: NativeIface });
}

// A native handle drops cleanly through the fast path, and the freed index
// is then invalid (already dropped).
{
  const { consume, dropRaw } = makeInstance().exports;
  const index = consume(new WebIDLNativeResource(1));
  assertEq(index !== 0, true);
  dropRaw(index);
  assertErrorMessage(() => dropRaw(index), Error,
                     /invalid component resource handle/);
}

// Index 0 (the reserved sentinel) and an out-of-bounds index both fall to
// the slow path and trap, exactly as for a Wrapped handle.
{
  const { dropRaw } = makeInstance().exports;
  assertErrorMessage(() => dropRaw(0), Error,
                     /invalid component resource handle/);
  assertErrorMessage(() => dropRaw(999999), Error,
                     /invalid component resource handle/);
}

// A table grown past its initial capacity still drops correctly: the fast
// path must reload entries_/freeStack_ after every growth, never bake a
// stale base pointer computed before the table last reallocated.
{
  const { consume, dropRaw } = makeInstance().exports;
  const indices = [];
  for (let i = 0; i < 64; i++) {
    indices.push(consume(new WebIDLNativeResource(i)));
  }
  assertEq(new Set(indices).size, indices.length);
  for (const index of indices) {
    dropRaw(index);
  }
  for (const index of indices) {
    assertErrorMessage(() => dropRaw(index), Error,
                       /invalid component resource handle/);
  }
}

// Mixing Native and Wrapped handles in the same table: the fast path's
// Present+Native check correctly routes the Wrapped entry to the slow path
// without disturbing its Native sibling, and vice versa.
{
  const { consume, dropRaw } = makeInstance().exports;
  const nativeIndex = consume(new WebIDLNativeResource(7));
  const wrappedIndex = consume({}); // not a WebIDLNativeResource -> Wrapped
  dropRaw(wrappedIndex);
  dropRaw(nativeIndex);
  assertErrorMessage(() => dropRaw(nativeIndex), Error,
                     /invalid component resource handle/);
  assertErrorMessage(() => dropRaw(wrappedIndex), Error,
                     /invalid component resource handle/);
}

// A *borrowed* handle drops the same way an owned one does:
// Instance::componentResourceDrop always passes requireOwn=false, since the
// component model's canon_resource_drop accepts borrows too.
{
  const { consumeBorrow, dropRaw } = makeInstance().exports;
  const index = consumeBorrow(new WebIDLNativeResource(2));
  dropRaw(index);
  assertErrorMessage(() => dropRaw(index), Error,
                     /invalid component resource handle/);
}
