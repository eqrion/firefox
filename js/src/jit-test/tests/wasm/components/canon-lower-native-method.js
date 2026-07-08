// |jit-test| skip-if: !wasmComponentsEnabled(); local-include:canon-instantiate-harness.js

// TryCallWebIDLNative now supports `[method]` imports: the receiver is just
// an ordinary leading interface param on both the component funcType and the
// native's own advertised overload (the Gecko generator already prepends
// `self` this way), so no special-casing is needed once the old bindsThis
// bail is gone. webidlTestNativeMethod is `self.getTagPlusN(n) -> self.tag +
// n`, a native fixture shaped exactly like this; getWebIDLTestNativeMethodCallCount
// distinguishes the native path firing from the generic JS::Call fallback.
//
// The resource's interface must be bound for the native path to engage at
// all (see the "statically doomed binding" trap otherwise) - done here by
// reusing the shell WebIDLInterface value itself as the `[method]` Ctor: it
// is a plain extensible JSObject, so giving it a `.prototype.getTagPlusN`
// satisfies Web.md's "Read the imports object" Ctor.prototype walk, while
// its class identity is what lets the interface-binding hook recognize it.

const MethodIface = getWebIDLTestMethodResourceInterface();
MethodIface.prototype = { getTagPlusN: webidlTestNativeMethod };

// Builds a fresh component (a Component's resource-interface binding is
// one-shot, so each scenario needs its own) importing resource type
// "my-method-resource" and `[method]my-method-resource.get-tag-plus-n`,
// registers `selfValue` as an own handle, then calls the method with that
// handle (index 1 - the only one added) and `n`. `resourceValue` is what's
// bound for "MyMethodResource" in the import object.
function callMethod(resourceValue, selfValue, n) {
  const component = new WebAssembly.Component(wasmTextToBinary(`
    (component
      (import "my-method-resource" (type $T (sub resource)))
      (import "[method]my-method-resource.get-tag-plus-n"
        (func $method (param "self" (borrow $T)) (param "n" s32) (result s32)))
      (core func $methodLowered (canon lower (func $method)))
      (core instance $env (export "get-tag-plus-n" (func $methodLowered)))

      (core module $impl
        (import "env" "get-tag-plus-n" (func $method (param i32 i32) (result i32)))
        ${CANON_HARNESS_MEMORY_REALLOC}
        (func (export "addHandle") (param $v i32))
        (func (export "callMethod") (param $self i32) (param $n i32) (result i32)
          (call $method (local.get $self) (local.get $n))))
      (core instance $implInst (instantiate $impl (with "env" (instance $env))))
      (alias core export $implInst "addHandle" (core func $addHandleF))
      (alias core export $implInst "mem" (core memory $mem))
      (alias core export $implInst "realloc" (core func $realloc))

      (type $tAdd (func (param "v" (own $T))))
      (func $liftedAdd (type $tAdd)
        (canon lift (core func $addHandleF) (memory $mem) (realloc $realloc)))
      (export "add-handle" (func $liftedAdd))
    )
  `));
  const instance = new WebAssembly.ComponentInstance(component, {
    MyMethodResource: resourceValue,
  });
  instance.exports.addHandle(selfValue);
  return wasmComponentCoreInstance(instance, 1).exports.callMethod(1, n);
}

// A same-origin WebIDLNativeResource, through the native-ops interface:
// AdoptJSObject classifies it Native. JS::ReadCoercedInterface must resolve
// it via reflectToJS (not object(), which would assert), so this is also the
// regression test for that fix.
{
  const before = getWebIDLTestNativeMethodCallCount();
  const result = callMethod(MethodIface, new WebIDLNativeResource(42), 5);
  assertEq(result, 47);
  assertEq(getWebIDLTestNativeMethodCallCount(), before + 1);
}

// A plain object through the same interface: unwrapNative can't recognize
// it, so it stays Wrapped - the other repr the receiver must handle.
{
  const before = getWebIDLTestNativeMethodCallCount();
  const result = callMethod(MethodIface, { tag: 100 }, 3);
  assertEq(result, 103);
  assertEq(getWebIDLTestNativeMethodCallCount(), before + 1);
}

// No interface bound (a plain JS constructor, as in
// name-mangling-resource-methods.js) - the resource canonicalizes to the
// generic `object` type, so the native path never has a typed self to match
// and every call falls back to JS::Call, binding `self` as `this`. The
// native call count must not move.
{
  function PlainResource() {}
  PlainResource.prototype.getTagPlusN = function(n) { return this.tag + n; };

  const before = getWebIDLTestNativeMethodCallCount();
  const result = callMethod(PlainResource, { tag: 9 }, 1);
  assertEq(result, 10);
  assertEq(getWebIDLTestNativeMethodCallCount(), before);
}
