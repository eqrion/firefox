// |jit-test| skip-if: !wasmComponentsEnabled(); local-include:canon-instantiate-harness.js

// canon lift over scalar component value types, through real instantiation.

function scalarIdentity(coreType) {
  return `${CANON_HARNESS_MEMORY_REALLOC}
    (func (export "f") (param $v ${coreType}) (result ${coreType}) (local.get $v))`;
}

function identityFn(componentTypeText, coreType) {
  return liftThrough(
    `(param "v" ${componentTypeText}) (result ${componentTypeText})`,
    scalarIdentity(coreType));
}

assertEq(identityFn("bool", "i32")(true), true);
assertEq(identityFn("bool", "i32")(false), false);

assertEq(identityFn("s8", "i32")(-42), -42);
assertEq(identityFn("u8", "i32")(200), 200);
assertEq(identityFn("s16", "i32")(-1234), -1234);
assertEq(identityFn("u16", "i32")(50000), 50000);
assertEq(identityFn("s32", "i32")(-70000), -70000);
assertEq(identityFn("u32", "i32")(70000), 70000);

// s64/u64 convert via ToNumber, not BigInt; only the core wasm call itself
// uses BigInt for i64.
assertEq(identityFn("s64", "i64")(-5), -5);
assertEq(identityFn("u64", "i64")(5), 5);

assertEq(identityFn("f32", "f32")(1.5), 1.5);
assertEq(identityFn("f64", "f64")(2.5), 2.5);

assertEq(identityFn("char", "i32")("A"), "A");

const stringIdentity = liftThrough(
  `(param "v" string) (result string)`,
  `${CANON_HARNESS_MEMORY_REALLOC}
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
     (local.set $retPtr (global.get $top))
     (global.set $top (i32.add (global.get $top) (i32.const 8)))
     (i32.store (local.get $retPtr) (local.get $dstPtr))
     (i32.store (i32.add (local.get $retPtr) (i32.const 4)) (local.get $len))
     (local.get $retPtr))`);
assertEq(stringIdentity("hello, component model"), "hello, component model");
assertEq(stringIdentity(""), "");
