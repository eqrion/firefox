// |jit-test| skip-if: !wasmComponentsEnabled()

// Web.md's "Read the imports object" resolves `[constructor]`/`[method]`/
// `[static]` function imports against the JS constructor already imported
// for their resource, rather than as flat properties on the import object:
// `[constructor]my-resource` -> the constructor itself, `[static]my-resource.
// make-default` -> `MyResource.makeDefault`, `[method]my-resource.get-child`
// -> `MyResource.prototype.getChild`. Only the resource's own import name
// ("my-resource" -> "MyResource") is read off the import object directly -
// resource/type names are PascalCase, matching the JS/WebIDL class-naming
// convention, unlike plain function/field names (see name-mangling.js).
//
// The func types here are trivial scalars: the bracket-tag grammar doesn't
// constrain the tagged function's signature (WasmValidate.cpp's
// DecodeComponentName only validates the name's syntax), so this mostly
// exercises name resolution, not resource own/borrow ABI plumbing (see
// canon-resources.js for that). The one exception is $methodImport, whose
// first param stands in for the canonical ABI's `borrow<self>` receiver:
// `canon lower` binds it as the resolved `MyResource.prototype.getChild`'s
// `this`, rather than passing it positionally like an ordinary param.

const component = new WebAssembly.Component(wasmTextToBinary(`
  (component
    (import "my-resource" (type $R (sub resource)))

    (type $tagged (func (param "n" s32)))
    (type $tagged_method (func (param "self" s32) (param "n" s32)))
    (import "[constructor]my-resource" (func $ctorImport (type $tagged)))
    (import "[method]my-resource.get-child" (func $methodImport (type $tagged_method)))
    (import "[static]my-resource.make-default" (func $staticImport (type $tagged)))

    (core func $ctorLowered (canon lower (func $ctorImport)))
    (core func $methodLowered (canon lower (func $methodImport)))
    (core func $staticLowered (canon lower (func $staticImport)))
    (core instance $env
      (export "ctor" (func $ctorLowered))
      (export "method" (func $methodLowered))
      (export "static" (func $staticLowered)))

    (core module $main
      (import "env" "ctor" (func $ctor (param i32)))
      (import "env" "method" (func $method (param i32 i32)))
      (import "env" "static" (func $static (param i32)))
      (func (export "call-ctor") (param $n i32) (call $ctor (local.get $n)))
      (func (export "call-method") (param $self i32) (param $n i32)
        (call $method (local.get $self) (local.get $n)))
      (func (export "call-static") (param $n i32) (call $static (local.get $n)))
    )
    (core instance (instantiate $main (with "env" (instance $env))))
  )
`));

// A plain function rather than an ES class: Web.md's "read the component
// function import" just invokes the JS value directly ("create a WebIDL
// operation for a JS callable" is explicitly TODO on whether that's a call or
// a construct - see Web.md line 324), so a class constructor would trap on
// invocation ("class constructors must be invoked with 'new'"). That's a
// pre-existing spec gap, not something this resolution change is meant to
// settle; a plain function sidesteps it while still exercising the same
// resource-association and camelCase-member resolution.
const calls = [];
function MyResource(n) { calls.push(["ctor", n]); }
MyResource.prototype.getChild = function(n) { calls.push(["method", this, n]); };
MyResource.makeDefault = function(n) { calls.push(["static", n]); };

const instance = new WebAssembly.ComponentInstance(component, {
  MyResource,
});
const core = wasmComponentCoreInstance(instance, 1);
core.exports["call-ctor"](1);
core.exports["call-method"](10, 2);
core.exports["call-static"](3);
assertEq(JSON.stringify(calls),
        JSON.stringify([["ctor", 1], ["method", 10, 2], ["static", 3]]));

// Without a JS value for "my-resource", the tagged imports have no
// constructor to resolve against, so they fall back to a plain lookup of
// their own (bracketed) name - which is never a property a caller would
// set, so this fails cleanly rather than silently misresolving.
assertErrorMessage(
  () => new WebAssembly.ComponentInstance(component, {}),
  WebAssembly.LinkError,
  /Function/);

// The resource's own name is PascalCase, not camelCase: providing the
// camelCase key doesn't satisfy the type import (no "MyResource" property),
// so it falls back the same way a missing import object would.
assertErrorMessage(
  () => new WebAssembly.ComponentInstance(component, { myResource: MyResource }),
  WebAssembly.LinkError,
  /Function/);
