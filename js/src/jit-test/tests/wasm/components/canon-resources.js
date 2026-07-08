// |jit-test| skip-if: !wasmComponentsEnabled(); local-include:canon-instantiate-harness.js

// canon lift/lower: imported resource handles (`own`/`borrow`), through real
// instantiation. Each group below sharing multiple exports shares one
// component instance (and so one handle table), mirroring how the
// pre-instantiation tests shared one shell ComponentHandleTable across
// several ComponentCanonLift/ComponentCanonLower calls.
//
// Every component instance always has its own handle table - there's no
// instantiation-reachable equivalent of "no handleTable" (which the removed
// ComponentCanonLift/ComponentCanonLower supported only as a testing
// convenience), so that case has no migration target here. Likewise, the
// low-level ComponentValue constructor's own inability to supply a handle
// table is a property of that (kept) testing built-in, not of instantiation,
// so it isn't re-tested here either.

const IDENTITY_IMPL = `${CANON_HARNESS_MEMORY_REALLOC}
  (func (export "f") (param $v i32) (result i32) (local.get $v))`;

// Lift-only round trip: a resource handle is allocated when the JS object is
// passed in as an `own<R>` param, then immediately consumed (lift_own) when
// the same value comes back as the `own<R>` result. Net handle-table usage
// is zero, and object identity is preserved.
{
  const fn = liftThrough(
    `(param "v" (own $T)) (result (own $T))`, IDENTITY_IMPL, { resourceImport: true });
  const obj = { tag: "resource-value" };
  assertEq(fn(obj), obj);
}

// borrow<R>: lift_borrow reads the handle without removing it, so this alone
// can't observe table state directly - see the "multiple simultaneous
// handles" group below for a case that does.
{
  const fn = liftThrough(
    `(param "v" (borrow $T)) (result (borrow $T))`, IDENTITY_IMPL, { resourceImport: true });
  const obj = {};
  assertEq(fn(obj), obj);
}

// Passing a non-object where an own/borrow handle is expected throws, rather
// than silently allocating a bogus handle.
{
  const fn = liftThrough(
    `(param "v" (own $T)) (result (own $T))`, IDENTITY_IMPL, { resourceImport: true });
  let threw = false;
  try {
    fn(5);
  } catch (e) {
    threw = true;
  }
  assertEq(threw, true);
}

// Mixed signature: an own<R> param sitting next to a plain scalar param.
// Checks that flat-value slot ordering isn't disturbed by a handle type;
// the result reads back flat slot 0 (the handle), ignoring slot 1 (the
// scalar) entirely.
{
  const fn = liftThrough(
    `(param "v" (own $T)) (param "n" s32) (result (own $T))`,
    `${CANON_HARNESS_MEMORY_REALLOC}
     (func (export "f") (param $a i32) (param $b i32) (result i32) (local.get $a))`,
    { resourceImport: true });
  const obj = {};
  assertEq(fn(obj, 999), obj);
}

// record{handle: own<R>, tag: s32}: two flat result values exceeds
// MAX_FLAT_RESULTS(1), so the record result spills to linear memory instead
// of the register-based flat path the earlier cases use.
{
  const fn = liftThrough(
    `(param "handle" (own $T)) (param "tag" s32)
     (result (record (field "handle" (own $T)) (field "tag" s32)))`,
    `${CANON_HARNESS_MEMORY_REALLOC}
     ;; record{handle, tag} identity: (handle, tag) -> retPtr to {handle@0 i32, tag@4 i32}
     (func (export "f") (param $handle i32) (param $tag i32) (result i32)
       (local $retPtr i32)
       (local.set $retPtr (global.get $top))
       (global.set $top (i32.add (global.get $top) (i32.const 8)))
       (i32.store (local.get $retPtr) (local.get $handle))
       (i32.store (i32.add (local.get $retPtr) (i32.const 4)) (local.get $tag))
       (local.get $retPtr))`,
    { resourceImport: true });
  const obj = {};
  const out = fn(obj, 42);
  assertEq(out.handle, obj);
  assertEq(out.tag, 42);
}

// list<own<R>>: each element marshals through linear memory as its own
// 4-byte handle index, not just a single scalar handle.
{
  const fn = liftThrough(
    `(param "v" (list (own $T))) (result (list (own $T)))`,
    `${CANON_HARNESS_MEMORY_REALLOC}
     ;; list<T> identity (T is 4 bytes, e.g. own/borrow's handle index):
     ;; (srcPtr, len) -> retPtr to a fresh (ptr, len) pair.
     (func (export "f") (param $srcPtr i32) (param $len i32) (result i32)
       (local $dstPtr i32)
       (local $retPtr i32)
       (local $i i32)
       (local.set $dstPtr (global.get $top))
       (global.set $top (i32.add (global.get $top) (i32.mul (local.get $len) (i32.const 4))))
       (block $done
         (loop $loop
           (br_if $done (i32.ge_u (local.get $i) (local.get $len)))
           (i32.store
             (i32.add (local.get $dstPtr) (i32.mul (local.get $i) (i32.const 4)))
             (i32.load (i32.add (local.get $srcPtr) (i32.mul (local.get $i) (i32.const 4)))))
           (local.set $i (i32.add (local.get $i) (i32.const 1)))
           (br $loop)))
       (local.set $retPtr (global.get $top))
       (global.set $top (i32.add (global.get $top) (i32.const 8)))
       (i32.store (local.get $retPtr) (local.get $dstPtr))
       (i32.store (i32.add (local.get $retPtr) (i32.const 4)) (local.get $len))
       (local.get $retPtr))`,
    { resourceImport: true });
  const objs = [{ id: 1 }, { id: 2 }, { id: 3 }];
  const result = fn(objs);
  assertEq(result.length, 3);
  assertEq(result[0], objs[0]);
  assertEq(result[1], objs[1]);
  assertEq(result[2], objs[2]);
}

// Full lift -> lower -> lift round trip, wiring a canon-lift export and a
// canon-lower import together through the SAME instance handle table, the
// way a real guest<->host resource handoff would work.
{
  const component = new WebAssembly.Component(wasmTextToBinary(`
    (component
      (import "r" (type $T (sub resource)))
      (import "callee" (func $callee (param "v" (own $T)) (result (own $T))))
      (core func $calleeLowered (canon lower (func $callee)))
      (core instance $env (export "callee" (func $calleeLowered)))

      (core module $impl
        (import "env" "callee" (func $callee (param i32) (result i32)))
        ${CANON_HARNESS_MEMORY_REALLOC}
        (func (export "add-handle") (param $v i32))
        (func (export "lowered") (param $v i32) (result i32) (call $callee (local.get $v)))
        (func (export "consume-handle") (result i32) (i32.const 1)))
      (core instance $implInst (instantiate $impl (with "env" (instance $env))))
      (alias core export $implInst "add-handle" (core func $addHandleF))
      (alias core export $implInst "lowered" (core func $loweredF))
      (alias core export $implInst "consume-handle" (core func $consumeHandleF))
      (alias core export $implInst "mem" (core memory $mem))
      (alias core export $implInst "realloc" (core func $realloc))

      (type $tAdd (func (param "v" (own $T))))
      (func $liftedAdd (type $tAdd)
        (canon lift (core func $addHandleF) (memory $mem) (realloc $realloc)))
      (export "add-handle" (func $liftedAdd))

      (type $tConsume (func (result (own $T))))
      (func $liftedConsume (type $tConsume)
        (canon lift (core func $consumeHandleF) (memory $mem) (realloc $realloc)))
      (export "consume-handle" (func $liftedConsume))
    )
  `));
  const obj = { tag: "round-trip" };
  const instance = new WebAssembly.ComponentInstance(component, { callee: (v) => v });
  // "lowered" stays at the raw core ABI boundary (an own<T> handle there is
  // just its i32 table index) - unlike add-handle/consume-handle, it isn't
  // wrapped in a second canon lift, which would expect a real JS object.
  const impl = wasmComponentCoreInstance(instance, 1).exports;

  instance.exports.addHandle(obj);
  assertEq(impl.lowered(1), 1); // index reused via the free-list
  assertEq(instance.exports.consumeHandle(), obj);
}

// Resource type mismatch: a handle allocated for one resource type traps
// when lifted as a different (nominally distinct) resource type.
{
  const component = new WebAssembly.Component(wasmTextToBinary(`
    (component
      (import "r1" (type $T1 (sub resource)))
      (import "r2" (type $T2 (sub resource)))
      (core module $impl
        ${CANON_HARNESS_MEMORY_REALLOC}
        (func (export "add-handle") (param $v i32))
        (func (export "mismatch-lift") (result i32) (i32.const 1)))
      (core instance $implInst (instantiate $impl))
      (alias core export $implInst "add-handle" (core func $addHandleF))
      (alias core export $implInst "mismatch-lift" (core func $mismatchF))
      (alias core export $implInst "mem" (core memory $mem))
      (alias core export $implInst "realloc" (core func $realloc))

      (type $tAdd (func (param "v" (own $T1))))
      (func $liftedAdd (type $tAdd)
        (canon lift (core func $addHandleF) (memory $mem) (realloc $realloc)))
      (export "add-handle" (func $liftedAdd))

      (type $tMismatch (func (result (own $T2))))
      (func $liftedMismatch (type $tMismatch)
        (canon lift (core func $mismatchF) (memory $mem) (realloc $realloc)))
      (export "mismatch-lift" (func $liftedMismatch))
    )
  `));
  const instance = new WebAssembly.ComponentInstance(component, {});
  instance.exports.addHandle({});
  let threw = false;
  try {
    instance.exports.mismatchLift();
  } catch (e) {
    threw = true;
  }
  assertEq(threw, true);
}

// GC stress: an object reachable only through a resource handle in the
// table survives collection.
{
  const component = new WebAssembly.Component(wasmTextToBinary(`
    (component
      (import "r" (type $T (sub resource)))
      (core module $impl
        ${CANON_HARNESS_MEMORY_REALLOC}
        (func (export "add-handle") (param $v i32))
        (func (export "read-back") (result i32) (i32.const 1)))
      (core instance $implInst (instantiate $impl))
      (alias core export $implInst "add-handle" (core func $addHandleF))
      (alias core export $implInst "read-back" (core func $readBackF))
      (alias core export $implInst "mem" (core memory $mem))
      (alias core export $implInst "realloc" (core func $realloc))

      (type $tAdd (func (param "v" (own $T))))
      (func $liftedAdd (type $tAdd)
        (canon lift (core func $addHandleF) (memory $mem) (realloc $realloc)))
      (export "add-handle" (func $liftedAdd))

      (type $tRead (func (result (own $T))))
      (func $liftedRead (type $tRead)
        (canon lift (core func $readBackF) (memory $mem) (realloc $realloc)))
      (export "read-back" (func $liftedRead))
    )
  `));
  const instance = new WebAssembly.ComponentInstance(component, {});
  instance.exports.addHandle({ marker: "gc-survivor" });
  gc();
  assertEq(instance.exports.readBack().marker, "gc-survivor");
}

// own/borrow flag mismatch: a handle allocated as `borrow` traps when
// lifted as `own`, even though the resource type matches. The handle is
// still removed from the table even though the call threw.
//
// Stale/invalid index traps: lifting an already-consumed index, an
// out-of-bounds index, and index 0 (the table's reserved sentinel) all
// trap. `lift-at-index` routes an arbitrary index through a plain s32 param
// instead of an object arg, by passing it straight through core wasm as if
// it were the already-flattened handle value.
{
  const component = new WebAssembly.Component(wasmTextToBinary(`
    (component
      (import "r" (type $T (sub resource)))
      (core module $impl
        ${CANON_HARNESS_MEMORY_REALLOC}
        (func (export "add-borrow") (param $v i32))
        (func (export "add-own") (param $v i32))
        (func (export "lift-at-index") (param $i i32) (result i32) (local.get $i)))
      (core instance $implInst (instantiate $impl))
      (alias core export $implInst "add-borrow" (core func $addBorrowF))
      (alias core export $implInst "add-own" (core func $addOwnF))
      (alias core export $implInst "lift-at-index" (core func $liftAtIndexF))
      (alias core export $implInst "mem" (core memory $mem))
      (alias core export $implInst "realloc" (core func $realloc))

      (type $tAddBorrow (func (param "v" (borrow $T))))
      (func $liftedAddBorrow (type $tAddBorrow)
        (canon lift (core func $addBorrowF) (memory $mem) (realloc $realloc)))
      (export "add-borrow" (func $liftedAddBorrow))

      (type $tAddOwn (func (param "v" (own $T))))
      (func $liftedAddOwn (type $tAddOwn)
        (canon lift (core func $addOwnF) (memory $mem) (realloc $realloc)))
      (export "add-own" (func $liftedAddOwn))

      (type $tLiftAsOwn (func (param "i" s32) (result (own $T))))
      (func $liftedLiftAsOwn (type $tLiftAsOwn)
        (canon lift (core func $liftAtIndexF) (memory $mem) (realloc $realloc)))
      (export "lift-as-own" (func $liftedLiftAsOwn))
    )
  `));

  // own/borrow flag mismatch.
  {
    const instance = new WebAssembly.ComponentInstance(component, {});
    instance.exports.addBorrow({});
    let threw = false;
    try {
      instance.exports.liftAsOwn(1);
    } catch (e) {
      threw = true;
    }
    assertEq(threw, true);
  }

  // Lifting an already-consumed index traps.
  {
    const instance = new WebAssembly.ComponentInstance(component, {});
    const obj = {};
    instance.exports.addOwn(obj);
    assertEq(instance.exports.liftAsOwn(1), obj);
    let threw = false;
    try {
      instance.exports.liftAsOwn(1); // already removed above
    } catch (e) {
      threw = true;
    }
    assertEq(threw, true);
  }

  // An index past the end of the table (out of bounds) traps.
  {
    const instance = new WebAssembly.ComponentInstance(component, {});
    let threw = false;
    try {
      instance.exports.liftAsOwn(999);
    } catch (e) {
      threw = true;
    }
    assertEq(threw, true);
  }

  // Index 0 is the table's reserved sentinel and never a real handle, even
  // once the table has real entries at other indices.
  {
    const instance = new WebAssembly.ComponentInstance(component, {});
    instance.exports.addOwn({});
    let threw = false;
    try {
      instance.exports.liftAsOwn(0);
    } catch (e) {
      threw = true;
    }
    assertEq(threw, true);
  }

  // Multiple simultaneous handles: distinct objects get distinct,
  // independent indices, and each can be looked up on its own.
  {
    const instance = new WebAssembly.ComponentInstance(component, {});
    const objA = {};
    const objB = {};
    instance.exports.addOwn(objA); // index 1
    instance.exports.addOwn(objB); // index 2
    assertEq(instance.exports.liftAsOwn(1), objA);
    assertEq(instance.exports.liftAsOwn(2), objB);
  }
}

// canon lower of a result nesting a no-WebIDL-interface own<R> (e.g.
// `option<own<R>>`, the shape of a `[method]` returning `option<some-
// resource>`): $T has no WebIDL interface bound, so it canonicalizes to
// WebIDLTypeKind::Object rather than Interface, but still round-trips
// through the handle table like any other own/borrow. option<own<R>>
// flattens to 2 core values, exceeding MAX_FLAT_RESULTS(1), so this also
// exercises the memory-spilled result path, with the lowered call's
// fused fast path (LoweredCall's WriteResultValueAtStep) reaching this
// nested handle through the Option step.
{
  const component = new WebAssembly.Component(wasmTextToBinary(`
    (component
      (import "r" (type $T (sub resource)))
      (import "callee" (func $callee (result (option (own $T)))))

      (core module $libc ${CANON_HARNESS_MEMORY_REALLOC})
      (core instance $libcInst (instantiate $libc))
      (alias core export $libcInst "mem" (core memory $mem))
      (alias core export $libcInst "realloc" (core func $realloc))

      (core func $calleeLowered
        (canon lower (func $callee) (memory $mem) (realloc $realloc)))
      (core instance $env (export "callee" (func $calleeLowered)))

      (core module $impl
        (import "libc" "mem" (memory 1))
        (import "libc" "realloc" (func $realloc (param i32 i32 i32 i32) (result i32)))
        (import "env" "callee" (func $callee (param i32)))
        (func (export "call") (result i32)
          (local $ptr i32)
          (local.set $ptr
            (call $realloc (i32.const 0) (i32.const 0) (i32.const 4) (i32.const 8)))
          (call $callee (local.get $ptr))
          (local.get $ptr)))
      (core instance $implInst
        (instantiate $impl
          (with "libc" (instance $libcInst))
          (with "env" (instance $env))))
      (alias core export $implInst "call" (core func $callF))

      (type $t (func (result (option (own $T)))))
      (func $lifted (type $t)
        (canon lift (core func $callF) (memory $mem) (realloc $realloc)))
      (export "call" (func $lifted))
    )
  `));

  // Some case: the callee's returned object round-trips through the handle
  // table and back out unchanged.
  {
    const obj = { tag: "some-resource" };
    const instance = new WebAssembly.ComponentInstance(component, {
      callee: () => obj,
    });
    assertEq(instance.exports.call(), obj);
  }

  // None case: no handle is ever allocated.
  {
    const instance = new WebAssembly.ComponentInstance(component, {
      callee: () => null,
    });
    assertEq(instance.exports.call(), null);
  }
}
