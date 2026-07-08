// |jit-test| skip-if: !wasmComponentsEnabled(); local-include:canon-instantiate-harness.js

// `variant`/`result` canonical-ABI value marshaling, through real
// instantiation: the in-memory spill path (exercised via a variant-typed
// function result, which always spills since MaxFlatResults=1) and the flat-
// register path, including the join-coercion between differently typed
// cases sharing a flat slot - e.g. `variant{a: s32, b: f32}` joins to a
// single i32 slot, so an `f32` case's payload must be bit-reinterpreted out
// of that shared i32 register. `result<T,E>` shares this same marshaling
// code, so isn't re-tested separately here.
//
// The mirror direction - passing a heterogeneous-payload variant as a
// canon.lift *parameter*, or returning one from canon.lower - isn't tested
// here: the former needs WebIDL union resolution that's a documented,
// pre-existing placeholder that can't reliably distinguish `long` from
// `unrestricted float`; the latter hits a separate, pre-existing bug in the
// result-direction join-coercion (unrelated to canon lower's result-spill
// out-param convention fixed alongside this migration).

// canon.lift: variant{a: s32, b: none} as a function's RESULT - spills to
// memory.
{
  const fn = liftThrough(
    `(param "disc" s32) (param "payload" s32)
     (result (variant (case "a" s32) (case "b")))`,
    `${CANON_HARNESS_MEMORY_REALLOC}
     ;; variant{a: s32, b: none} identity: (disc, payload) -> retPtr to
     ;; {disc@0 u8, payload@4 i32}, matching Alignment/ElemSize for this
     ;; variant (discSize=1, max payload align/size=4 -> 8 bytes total).
     (func (export "f") (param $disc i32) (param $payload i32) (result i32)
       (local $retPtr i32)
       (local.set $retPtr (global.get $top))
       (global.set $top (i32.add (global.get $top) (i32.const 8)))
       (i32.store8 (local.get $retPtr) (local.get $disc))
       (i32.store (i32.add (local.get $retPtr) (i32.const 4)) (local.get $payload))
       (local.get $retPtr))`);

  const a = fn(0, 42);
  assertEq(a.kind, "a");
  assertEq(a.value, 42);

  const b = fn(1, 0);
  assertEq(b.kind, "b");
  assertEq("value" in b, false);
}

// canon.lower: variant{a: s32, b: f32} as a PARAM - flat (no memory, since
// this variant fits well under MAX_FLAT_PARAMS), exercising the
// join-coercion. This direction is safe from the union ambiguity noted
// above: params flow core -> component -> WebIDL -> JS, and the WebIDL step
// always knows the payload's exact concrete type already. The raw core arg
// for the "b" case is the literal i32 bit pattern of the f32 value 2.5,
// which must be reinterpreted back to f32.
{
  let seenArg;
  const { caller } = lowerThrough(
    `(param "v" (variant (case "a" s32) (case "b" f32))) (result f64)`,
    (v) => { seenArg = v; return v.value; },
    `(import "env" "f" (func $f (param i32 i32) (result f64)))
     (export "f" (func $f))`);

  assertEq(caller.f(0, 5), 5);
  assertEq(seenArg.kind, "a");
  assertEq(seenArg.value, 5);

  const bitsOf2_5AsF32 = new Int32Array(new Float32Array([2.5]).buffer)[0];
  assertEq(caller.f(1, bitsOf2_5AsF32), 2.5);
  assertEq(seenArg.kind, "b");
  assertEq(seenArg.value, 2.5);
}
