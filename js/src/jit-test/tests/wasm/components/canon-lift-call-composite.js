// |jit-test| skip-if: !wasmComponentsEnabled(); local-include:canon-instantiate-harness.js

// canon lift over composite component value types, plus the
// >MAX_FLAT_PARAMS memory-spill path, through real instantiation.

// list<s32>
{
  const fn = liftThrough(
    `(param "v" (list s32)) (result (list s32))`,
    `${CANON_HARNESS_MEMORY_REALLOC}
     ;; list<s32> identity: (srcPtr, len) -> retPtr to a fresh (ptr, len) pair.
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
       (local.get $retPtr))`);
  assertEq(JSON.stringify(fn([1, 2, 3, -4, 5])), JSON.stringify([1, 2, 3, -4, 5]));
  assertEq(JSON.stringify(fn([])), JSON.stringify([]));
}

// enum
{
  const fn = liftThrough(
    `(param "v" (enum "a" "b" "c")) (result (enum "a" "b" "c"))`,
    `${CANON_HARNESS_MEMORY_REALLOC}
     (func (export "f") (param $v i32) (result i32) (local.get $v))`);
  assertEq(fn("b"), "b");
  assertEq(fn("a"), "a");
}

// flags
{
  const fn = liftThrough(
    `(param "v" (flags "r" "w" "x" "extra1" "extra2"))
     (result (flags "r" "w" "x" "extra1" "extra2"))`,
    `${CANON_HARNESS_MEMORY_REALLOC}
     (func (export "f") (param $v i32) (result i32) (local.get $v))`);
  const value = { r: true, w: false, x: true, extra1: false, extra2: true };
  assertEq(JSON.stringify(fn(value)), JSON.stringify(value));
}

// record
{
  const fn = liftThrough(
    `(param "a" s32) (param "b" f64)
     (result (record (field "a" s32) (field "b" f64)))`,
    `${CANON_HARNESS_MEMORY_REALLOC}
     ;; record{a:s32, b:f64} identity: (a, b) -> retPtr to {a@0 i32, b@8 f64}
     (func (export "f") (param $a i32) (param $b f64) (result i32)
       (local $retPtr i32)
       (local.set $retPtr (global.get $top))
       (global.set $top (i32.add (global.get $top) (i32.const 16)))
       (i32.store (local.get $retPtr) (local.get $a))
       (f64.store (i32.add (local.get $retPtr) (i32.const 8)) (local.get $b))
       (local.get $retPtr))`);
  const out = fn(7, 3.5);
  assertEq(out.a, 7);
  assertEq(out.b, 3.5);
}

// tuple
{
  const fn = liftThrough(
    `(param "a" s32) (param "b" s32) (result (tuple s32 s32))`,
    `${CANON_HARNESS_MEMORY_REALLOC}
     ;; tuple<s32,s32> identity: (a, b) -> retPtr to {a@0 i32, b@4 i32}
     (func (export "f") (param $a i32) (param $b i32) (result i32)
       (local $retPtr i32)
       (local.set $retPtr (global.get $top))
       (global.set $top (i32.add (global.get $top) (i32.const 8)))
       (i32.store (local.get $retPtr) (local.get $a))
       (i32.store (i32.add (local.get $retPtr) (i32.const 4)) (local.get $b))
       (local.get $retPtr))`);
  const out = fn(10, 20);
  assertEq(out[0], 10);
  assertEq(out[1], 20);
}

// option
{
  const fn = liftThrough(
    `(param "disc" s32) (param "payload" s32) (result (option s32))`,
    `${CANON_HARNESS_MEMORY_REALLOC}
     ;; option<s32> identity: (disc, payload) -> retPtr to {disc@0 u8, payload@4 i32}
     (func (export "f") (param $disc i32) (param $payload i32) (result i32)
       (local $retPtr i32)
       (local.set $retPtr (global.get $top))
       (global.set $top (i32.add (global.get $top) (i32.const 8)))
       (i32.store8 (local.get $retPtr) (local.get $disc))
       (i32.store (i32.add (local.get $retPtr) (i32.const 4)) (local.get $payload))
       (local.get $retPtr))`);
  assertEq(fn(1, 42), 42);
  assertEq(fn(0, 0), null);
}

// >MAX_FLAT_PARAMS(16) params: spills to a single pointer param.
{
  const params17 = Array.from({ length: 17 }, (_, i) => `(param "p${i}" s32)`).join(" ");
  const fn = liftThrough(
    `${params17} (result s32)`,
    `${CANON_HARNESS_MEMORY_REALLOC}
     ;; Sums 17 s32 params, passed as a single pointer to a tuple-of-17-s32
     ;; laid out at 4-byte stride.
     (func (export "f") (param $ptr i32) (result i32)
       (local $i i32)
       (local $sum i32)
       (block $done
         (loop $loop
           (br_if $done (i32.ge_u (local.get $i) (i32.const 17)))
           (local.set $sum (i32.add (local.get $sum)
             (i32.load (i32.add (local.get $ptr) (i32.mul (local.get $i) (i32.const 4))))))
           (local.set $i (i32.add (local.get $i) (i32.const 1)))
           (br $loop)))
       (local.get $sum))`);
  const args = [];
  let expected = 0;
  for (let i = 0; i < 17; i++) {
    args.push(i + 1);
    expected += i + 1;
  }
  assertEq(fn(...args), expected);
}

// Nested composite: record{name: string, values: list<s32>}.
{
  const fn = liftThrough(
    `(result (record (field "name" string) (field "values" (list s32))))`,
    `${CANON_HARNESS_MEMORY_REALLOC}
     ;; Returns a {name: string, values: list<s32>} record laid out as
     ;; {namePtr@0, nameLen@4, valuesPtr@8, valuesLen@12}.
     (func (export "f") (result i32)
       (local $namePtr i32)
       (local $valuesPtr i32)
       (local $retPtr i32)
       (local.set $namePtr (global.get $top))
       (global.set $top (i32.add (global.get $top) (i32.const 2)))
       (i32.store8 (local.get $namePtr) (i32.const 97)) ;; 'a'
       (i32.store8 (i32.add (local.get $namePtr) (i32.const 1)) (i32.const 98)) ;; 'b'
       (global.set $top
         (i32.and (i32.add (global.get $top) (i32.const 3)) (i32.const -4)))
       (local.set $valuesPtr (global.get $top))
       (global.set $top (i32.add (global.get $top) (i32.const 12)))
       (i32.store (local.get $valuesPtr) (i32.const 1))
       (i32.store (i32.add (local.get $valuesPtr) (i32.const 4)) (i32.const 2))
       (i32.store (i32.add (local.get $valuesPtr) (i32.const 8)) (i32.const 3))
       (local.set $retPtr (global.get $top))
       (global.set $top (i32.add (global.get $top) (i32.const 16)))
       (i32.store (local.get $retPtr) (local.get $namePtr))
       (i32.store (i32.add (local.get $retPtr) (i32.const 4)) (i32.const 2))
       (i32.store (i32.add (local.get $retPtr) (i32.const 8)) (local.get $valuesPtr))
       (i32.store (i32.add (local.get $retPtr) (i32.const 12)) (i32.const 3))
       (local.get $retPtr))`);
  const result = fn();
  assertEq(result.name, "ab");
  assertEq(JSON.stringify(result.values), JSON.stringify([1, 2, 3]));
}
