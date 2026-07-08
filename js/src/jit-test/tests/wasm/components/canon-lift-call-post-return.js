// |jit-test| skip-if: !wasmComponentsEnabled(); local-include:canon-instantiate-harness.js

// canon lift: the optional `postReturn` hook is called exactly once per call
// with the callee's raw core result. Omitting it is still valid. Through
// real instantiation.

const scalarImpl = `${CANON_HARNESS_MEMORY_REALLOC}
  (global $postReturnCount (mut i32) (i32.const 0))
  (global $postReturnLastArg (mut i32) (i32.const -1))
  (func (export "f") (param $v i32) (result i32) (local.get $v))
  (func (export "cabi_post") (param $v i32)
    (global.set $postReturnCount (i32.add (global.get $postReturnCount) (i32.const 1)))
    (global.set $postReturnLastArg (local.get $v)))
  (func (export "postReturnCount") (result i32) (global.get $postReturnCount))
  (func (export "postReturnLastArg") (result i32) (global.get $postReturnLastArg))`;

{
  const component = new WebAssembly.Component(wasmTextToBinary(`
    (component
      (core module $impl ${scalarImpl})
      (core instance $implInst (instantiate $impl))
      (alias core export $implInst "f" (core func $implF))
      (alias core export $implInst "mem" (core memory $mem))
      (alias core export $implInst "realloc" (core func $realloc))
      (alias core export $implInst "cabi_post" (core func $postReturn))

      (type $t (func (param "v" s32) (result s32)))
      (func $lifted (type $t)
        (canon lift (core func $implF) (memory $mem) (realloc $realloc) (post-return (func $postReturn))))
      (export "f" (func $lifted))
    )
  `));
  const instance = new WebAssembly.ComponentInstance(component, {});
  const counts = wasmComponentCoreInstance(instance, 0).exports;

  assertEq(instance.exports.f(42), 42);
  assertEq(counts.postReturnCount(), 1);
  assertEq(counts.postReturnLastArg(), 42);

  assertEq(instance.exports.f(-7), -7);
  assertEq(counts.postReturnCount(), 2);
  assertEq(counts.postReturnLastArg(), -7);
}

// Omitting postReturn is still valid.
{
  const fnNoPostReturn = liftThrough(
    `(param "v" s32) (result s32)`,
    scalarImpl);
  assertEq(fnNoPostReturn(99), 99);
}

// Spilled (>MAX_FLAT_RESULTS) results: post_return receives the collapsed
// pointer, not the original decomposed (ptr, len) pair.
{
  const strImpl = `${CANON_HARNESS_MEMORY_REALLOC}
    (global $postReturnCount (mut i32) (i32.const 0))
    (global $postReturnLastArg (mut i32) (i32.const -1))
    ;; string identity: (srcPtr, len) -> retPtr to a fresh (ptr, len) pair.
    (func (export "f") (param $srcPtr i32) (param $len i32) (result i32)
      (local $dstPtr i32)
      (local $retPtr i32)
      (local $i i32)
      (local.set $dstPtr (global.get $top))
      (global.set $top (i32.add (global.get $top) (local.get $len)))
      (block $done
        (loop $loop
          (br_if $done (i32.ge_u (local.get $i) (local.get $len)))
          (i32.store8
            (i32.add (local.get $dstPtr) (local.get $i))
            (i32.load8_u (i32.add (local.get $srcPtr) (local.get $i))))
          (local.set $i (i32.add (local.get $i) (i32.const 1)))
          (br $loop)))
      (global.set $top
        (i32.and (i32.add (global.get $top) (i32.const 3)) (i32.const -4)))
      (local.set $retPtr (global.get $top))
      (global.set $top (i32.add (global.get $top) (i32.const 8)))
      (i32.store (local.get $retPtr) (local.get $dstPtr))
      (i32.store (i32.add (local.get $retPtr) (i32.const 4)) (local.get $len))
      (local.get $retPtr))
    (func (export "cabi_post") (param $retPtr i32)
      (global.set $postReturnCount (i32.add (global.get $postReturnCount) (i32.const 1)))
      (global.set $postReturnLastArg (local.get $retPtr)))
    (func (export "postReturnCount") (result i32) (global.get $postReturnCount))
    (func (export "postReturnLastArg") (result i32) (global.get $postReturnLastArg))`;

  const component = new WebAssembly.Component(wasmTextToBinary(`
    (component
      (core module $impl ${strImpl})
      (core instance $implInst (instantiate $impl))
      (alias core export $implInst "f" (core func $implF))
      (alias core export $implInst "mem" (core memory $mem))
      (alias core export $implInst "realloc" (core func $realloc))
      (alias core export $implInst "cabi_post" (core func $postReturn))

      (type $t (func (param "v" string) (result string)))
      (func $lifted (type $t)
        (canon lift (core func $implF) (memory $mem) (realloc $realloc) (post-return (func $postReturn))))
      (export "f" (func $lifted))
    )
  `));
  const instance = new WebAssembly.ComponentInstance(component, {});
  const counts = wasmComponentCoreInstance(instance, 0).exports;

  assertEq(instance.exports.f("hi"), "hi");
  assertEq(counts.postReturnCount(), 1);
  assertEq(counts.postReturnLastArg() > 0, true);

  assertEq(instance.exports.f("bye"), "bye");
  assertEq(counts.postReturnCount(), 2);
}

// Spilled record result: post_return receives the collapsed pointer to a
// {a@0 i32, b@8 f64} layout.
{
  const recImpl = `${CANON_HARNESS_MEMORY_REALLOC}
    (global $postReturnCount (mut i32) (i32.const 0))
    (global $postReturnLastArg (mut i32) (i32.const -1))
    ;; record{a:s32, b:f64} identity: (a, b) -> retPtr to {a@0 i32, b@8 f64}
    (func (export "f") (param $a i32) (param $b f64) (result i32)
      (local $retPtr i32)
      (local.set $retPtr (global.get $top))
      (global.set $top (i32.add (global.get $top) (i32.const 16)))
      (i32.store (local.get $retPtr) (local.get $a))
      (f64.store (i32.add (local.get $retPtr) (i32.const 8)) (local.get $b))
      (local.get $retPtr))
    (func (export "cabi_post") (param $retPtr i32)
      (global.set $postReturnCount (i32.add (global.get $postReturnCount) (i32.const 1)))
      (global.set $postReturnLastArg (local.get $retPtr)))
    (func (export "postReturnCount") (result i32) (global.get $postReturnCount))
    (func (export "postReturnLastArg") (result i32) (global.get $postReturnLastArg))`;

  const component = new WebAssembly.Component(wasmTextToBinary(`
    (component
      (core module $impl ${recImpl})
      (core instance $implInst (instantiate $impl))
      (alias core export $implInst "f" (core func $implF))
      (alias core export $implInst "mem" (core memory $mem))
      (alias core export $implInst "realloc" (core func $realloc))
      (alias core export $implInst "cabi_post" (core func $postReturn))

      (type $t (func (param "a" s32) (param "b" f64) (result (record (field "a" s32) (field "b" f64)))))
      (func $lifted (type $t)
        (canon lift (core func $implF) (memory $mem) (realloc $realloc) (post-return (func $postReturn))))
      (export "f" (func $lifted))
    )
  `));
  const instance = new WebAssembly.ComponentInstance(component, {});
  const counts = wasmComponentCoreInstance(instance, 0).exports;

  const out = instance.exports.f(7, 3.5);
  assertEq(out.a, 7);
  assertEq(out.b, 3.5);
  assertEq(counts.postReturnCount(), 1);
  assertEq(counts.postReturnLastArg() > 0, true);
}
