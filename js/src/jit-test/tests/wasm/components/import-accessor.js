// |jit-test| skip-if: !wasmComponentsEnabled()

// A `[method]` import resolves against `Ctor.prototype` (see
// name-mangling-resource-methods.js). If the resolved property is a
// getter/setter accessor - e.g. a WebIDL attribute like
// `Document.prototype.body` - naively reading it would invoke the accessor
// right there with `this` == the prototype, which throws for a native
// getter/setter that expects a real receiver. Instead, ReadComponentFuncImport
// must capture the getter or setter function itself, so it can later be
// invoked with the real `self` receiver bound by canon lower's `[method]`
// binding. It picks getter vs setter by matching the component import's func
// type shape (0 params + a result is a getter; 1 param + no result is a
// setter, not counting the `[method]` self param) and must find an accessor
// inherited from a parent prototype, not just an own property.
//
// As in name-mangling-resource-methods.js, `self` here is a plain s32 (not a
// real own/borrow resource handle) - this exercises accessor resolution and
// this-binding, not the resource ABI.

const calls = [];

function Base() {}
Object.defineProperty(Base.prototype, "value", {
  configurable: true,
  get() { calls.push(["get", this]); return 42; },
  set(v) { calls.push(["set", this, v]); },
});

function MyResource() {}
Object.setPrototypeOf(MyResource.prototype, Base.prototype);

function buildAccessorComponent(funcTypeText, coreSigText) {
  return new WebAssembly.Component(wasmTextToBinary(`
    (component
      (import "my-resource" (type $R (sub resource)))
      (type $tagged (func ${funcTypeText}))
      (import "[method]my-resource.value" (func $accessorImport (type $tagged)))
      (core func $lowered (canon lower (func $accessorImport)))
      (core instance $env (export "accessor" (func $lowered)))
      (core module $main
        (import "env" "accessor" (func $accessor ${coreSigText}))
        (func (export "call") (param $self i32) (param $n i32) ${
          coreSigText.includes("result")
            ? '(result i32) (call $accessor (local.get $self))'
            : '(call $accessor (local.get $self) (local.get $n))'
        })
      )
      (core instance (instantiate $main (with "env" (instance $env))))
    )
  `));
}

// The getter: inherited from Base.prototype, found via the prototype chain
// walk, and matched by shape (0 params, a result).
{
  const component = buildAccessorComponent(
    `(param "self" s32) (result s32)`, `(param i32) (result i32)`);
  const instance = new WebAssembly.ComponentInstance(component, { MyResource });
  const core = wasmComponentCoreInstance(instance, 1);
  const got = core.exports.call(7, 0);
  assertEq(got, 42);
  assertEq(JSON.stringify(calls.pop()), JSON.stringify(["get", 7]));
}

// The setter: same inherited accessor, matched by shape (1 param, no result).
{
  const component = buildAccessorComponent(
    `(param "self" s32) (param "n" s32)`, `(param i32 i32)`);
  const instance = new WebAssembly.ComponentInstance(component, { MyResource });
  const core = wasmComponentCoreInstance(instance, 1);
  core.exports.call(7, 99);
  assertEq(JSON.stringify(calls.pop()), JSON.stringify(["set", 7, 99]));
}

// A getter-only accessor against a setter-shaped import: the shapes don't
// match, so resolution falls back to a plain property read - which, since
// this property has no setter but does have a getter, still invokes the
// getter (this is unrelated to and unfixed by this shape-matching; it's
// standard JS property-read semantics). The getter's return value (a number)
// isn't callable, so instantiation fails the same way any other
// non-function import would.
{
  function ReadOnly() {}
  Object.defineProperty(ReadOnly.prototype, "value", {
    configurable: true,
    get() { return 42; },
  });
  const component = buildAccessorComponent(
    `(param "self" s32) (param "n" s32)`, `(param i32 i32)`);
  assertErrorMessage(
    () => new WebAssembly.ComponentInstance(component, { MyResource: ReadOnly }),
    WebAssembly.LinkError,
    /Function/);
}
