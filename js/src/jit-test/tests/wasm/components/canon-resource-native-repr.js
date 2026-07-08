// |jit-test| skip-if: !wasmComponentsEnabled(); local-include:canon-instantiate-harness.js

// A resource declaring an interface with native ops
// (WebIDLInterface(name, parent, true)) classifies an incoming
// WebIDLNativeResource instance as `Native`; anything else (a plain
// object, a cross-compartment instance, or an interface with no native ops
// at all) stays `Wrapped`. Through real instantiation, binding the
// interface to an imported resource type (see canon-resource-interfaces.js
// for the general mechanism).
//
// The two reprs are distinguished observably by reflectToJS's identity
// behavior: `Wrapped` returns the exact same JSObject every time; `Native`
// calls the interface's `reflect` op, producing a fresh object (with
// matching data) each time.

const NativeIface = new WebIDLInterface("NativeIface", null, true);
const NoOpsIface = new WebIDLInterface("NoOpsIface");

// Lift `obj` through an `own<R>` param (allocates a handle), then
// immediately lower it back out through the same instance's handle table.
// Returns the object the lowered callee observed. `resourceIface`, if given,
// is bound to the imported resource type "r".
function ownRoundTrip(obj, resourceIface) {
  const component = new WebAssembly.Component(wasmTextToBinary(`
    (component
      (import "r" (type $T (sub resource)))
      (import "callee" (func $callee (param "v" (own $T))))
      (core func $calleeLowered (canon lower (func $callee)))
      (core instance $env (export "callee" (func $calleeLowered)))

      (core module $impl
        (import "env" "callee" (func $callee (param i32)))
        ${CANON_HARNESS_MEMORY_REALLOC}
        (func (export "add-handle") (param $v i32))
        (func (export "lowered") (param $v i32) (call $callee (local.get $v))))
      (core instance $implInst (instantiate $impl (with "env" (instance $env))))
      (alias core export $implInst "add-handle" (core func $addHandleF))
      (alias core export $implInst "mem" (core memory $mem))
      (alias core export $implInst "realloc" (core func $realloc))

      (type $tAdd (func (param "v" (own $T))))
      (func $liftedAdd (type $tAdd)
        (canon lift (core func $addHandleF) (memory $mem) (realloc $realloc)))
      (export "add-handle" (func $liftedAdd))
    )
  `));
  let seen;
  const imports = { callee: (v) => { seen = v; } };
  if (resourceIface) {
    imports.R = resourceIface;
  }
  const instance = new WebAssembly.ComponentInstance(component, imports);
  instance.exports.addHandle(obj);
  // "lowered" stays at the raw core ABI boundary - see canon-resources.js.
  wasmComponentCoreInstance(instance, 1).exports.lowered(1);
  return seen;
}

// Same-origin + native-ops interface + a real WebIDLNativeResource: Native.
// reflectToJS produces a fresh object each time, with the right tag.
{
  const native = new WebIDLNativeResource(42);
  const seen1 = ownRoundTrip(native, NativeIface);
  assertEq(seen1 !== native, true); // fresh object, not the original wrapper
  assertEq(seen1.tag, 42);

  const seen2 = ownRoundTrip(native, NativeIface);
  assertEq(seen2 !== native, true);
  assertEq(seen2 !== seen1, true); // fresh again, not the same as last time
  assertEq(seen2.tag, 42);
}

// A plain object (not a WebIDLNativeResource): unwrapNative can't recognize
// it, so it falls back to Wrapped - identity-preserving.
{
  const obj = { tag: "plain" };
  const seen = ownRoundTrip(obj, NativeIface);
  assertEq(seen, obj);
}

// The declared interface has no native ops at all: Wrapped, even though the
// object itself is a real WebIDLNativeResource that could otherwise unwrap.
{
  const native = new WebIDLNativeResource(7);
  const seen = ownRoundTrip(native, NoOpsIface);
  assertEq(seen, native);
}

// No declared interface (the generic `object` resource type): still
// Wrapped, unaffected by any of the above.
{
  const native = new WebIDLNativeResource(7);
  const seen = ownRoundTrip(native, null);
  assertEq(seen, native);
}

// Cross-compartment: same native-ops interface and a real
// WebIDLNativeResource, but constructed in a different global. isSameOrigin
// (compartment identity, this prototype's proxy for it) is false, so this
// stays Wrapped even though the unwrap would otherwise succeed.
{
  const g = newGlobal({ newCompartment: true });
  const foreign = g.eval("new WebIDLNativeResource(99)");
  const seen = ownRoundTrip(foreign, NativeIface);
  assertEq(seen, foreign);
}
