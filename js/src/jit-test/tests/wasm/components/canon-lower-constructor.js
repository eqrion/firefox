// |jit-test| skip-if: !wasmComponentsEnabled()

// A `[constructor]` import resolves to the interface object itself (see
// ReadComponentFuncImport), so `canon lower` has to *construct* it, not call
// it. WebIDL constructors - and ES class constructors, which stand in for them
// here - throw "class constructors must be invoked with 'new'" when merely
// called, so before this an ES class could not be used as a resource
// constructor at all (see the comment in name-mangling-resource-methods.js).

const ctorCalls = [];

class MyResource {
  constructor(n) {
    ctorCalls.push(n);
    this.n = n;
  }
  getN() {
    return this.n * 10;
  }
}

// Pins down what the rest of this test rests on: this callee cannot be
// invoked as a plain call at all, so reaching its body proves a construct.
assertErrorMessage(() => MyResource(1), TypeError, /without 'new'|with 'new'/);
assertEq(ctorCalls.length, 0);

const component = new WebAssembly.Component(wasmTextToBinary(`
  (component
    (import "my-resource" (type $T (sub resource)))
    (import "[constructor]my-resource"
      (func $ctor (param "n" s32) (result (own $T))))
    (import "[method]my-resource.get-n"
      (func $getN (param "self" (borrow $T)) (result s32)))
    (core func $ctorLowered (canon lower (func $ctor)))
    (core func $getNLowered (canon lower (func $getN)))
    (core instance $env
      (export "ctor" (func $ctorLowered))
      (export "get-n" (func $getNLowered)))

    (core module $impl
      (import "env" "ctor" (func $ctor (param i32) (result i32)))
      (import "env" "get-n" (func $getN (param i32) (result i32)))
      (func (export "make") (param $n i32) (result i32)
        (call $ctor (local.get $n)))
      (func (export "getN") (param $h i32) (result i32)
        (call $getN (local.get $h))))
    (core instance $implInst (instantiate $impl (with "env" (instance $env))))
  )
`));

const instance = new WebAssembly.ComponentInstance(component, {
  MyResource,
});
// Core instance indices in declaration order: the inline-export $env is 0,
// $implInst is 1.
const core = wasmComponentCoreInstance(instance, 1);

// Constructing is what proves the point: were this a JS::Call, the class
// constructor would throw before ctorCalls ever grew.
const handle = core.exports.make(7);
assertEq(JSON.stringify(ctorCalls), "[7]");

// The constructed instance became the `own<$T>` result, as a real handle - so
// a `[method]` call on that handle reaches the same object, with the right
// `this`.
assertEq(core.exports.getN(handle), 70);

// A second construct is a distinct instance, not a cached one.
const handle2 = core.exports.make(3);
assertEq(JSON.stringify(ctorCalls), "[7,3]");
assertEq(core.exports.getN(handle2), 30);
assertEq(handle === handle2, false);
