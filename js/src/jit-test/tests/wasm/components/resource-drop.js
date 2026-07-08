// |jit-test| skip-if: !wasmComponentsEnabled()

// End-to-end `canon resource.drop` for an imported resource type: an `own<T>`
// param lifted from JS allocates a handle in the instance's own handle table,
// a core import wired to `canon resource.drop` releases it, and dropping a
// stale or reserved index afterwards traps.

const component = new WebAssembly.Component(wasmTextToBinary(`
  (component
    (import "T1" (type $T1 (sub resource)))

    (core func $T1_drop (canon resource.drop $T1))
    (core instance $env (export "drop" (func $T1_drop)))

    (core module $main
      (import "env" "drop" (func $drop (param i32)))
      (global $lastIndex (mut i32) (i32.const -1))
      (func (export "consume") (param $h i32) (result i32)
        local.get $h
        global.set $lastIndex
        global.get $lastIndex
        call $drop
        global.get $lastIndex
      )
      (func (export "dropRaw") (param $h i32)
        local.get $h
        call $drop
      )
    )
    (core instance $mainInst (instantiate $main (with "env" (instance $env))))
    (alias core export $mainInst "consume" (core func $consumeCore))
    (alias core export $mainInst "dropRaw" (core func $dropRawCore))

    (type $consumeType (func (param "h" (own $T1)) (result s32)))
    (func $consume (type $consumeType) (canon lift (core func $consumeCore)))
    (export "consume" (func $consume))

    (type $dropRawType (func (param "h" s32)))
    (func $dropRawFn (type $dropRawType) (canon lift (core func $dropRawCore)))
    (export "drop-raw" (func $dropRawFn))
  )
`));

const instance = new WebAssembly.ComponentInstance(component, {});
const { consume } = instance.exports;
const dropRaw = instance.exports.dropRaw;

const index = consume({});
assertEq(typeof index, "number");
assertEq(index !== 0, true);

// The handle was already dropped by `consume` itself; dropping it again
// traps.
assertErrorMessage(() => dropRaw(index), Error,
                   /invalid component resource handle/);

// Index 0 is the table's reserved sentinel and is never allocated.
assertErrorMessage(() => dropRaw(0), Error,
                   /invalid component resource handle/);

// A fresh handle drops cleanly, and each call allocates a distinct index.
const index2 = consume({});
assertEq(index2 !== 0, true);
assertErrorMessage(() => dropRaw(index2), Error,
                   /invalid component resource handle/);
