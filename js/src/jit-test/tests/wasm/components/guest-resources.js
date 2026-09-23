// |jit-test| skip-if: !wasmComponentsEnabled()

// Resources of resource types defined by a component, whose values are guest
// resource instances. The JS-API behavior is also covered by the WPT tests in
// wasm/component/jsapi.

// $R's rep is the argument to its constructor, and its destructor calls
// `dropped` with the rep, or traps if `dtorTraps`.
function instantiate({ dtorTraps = false, dropped = () => {} } = {}) {
  return new WebAssembly.ComponentInstance(
    new WebAssembly.Component(wasmTextToBinary(`(component
      (import "dropped" (func $dropped (param "rep" s32)))
      (core func $dropped' (canon lower (func $dropped)))
      (core module $Dtor
        (import "" "dropped" (func $dropped (param i32)))
        (func (export "dtor") (param i32)
          ${dtorTraps ? "unreachable" : "(call $dropped (local.get 0))"}))
      (core instance $dtor (instantiate $Dtor
        (with "" (instance (export "dropped" (func $dropped'))))))
      (type $R (resource (rep i32) (dtor (core func $dtor "dtor"))))
      (type $Hidden (resource (rep i32)))
      (core func $R.new (canon resource.new $R))
      (core func $R.rep (canon resource.rep $R))
      (core func $Hidden.new (canon resource.new $Hidden))
      (core module $M
        (import "" "R.new" (func $R.new (param i32) (result i32)))
        (import "" "R.rep" (func $R.rep (param i32) (result i32)))
        (import "" "Hidden.new" (func $Hidden.new (param i32) (result i32)))
        (func (export "new") (param i32) (result i32) (call $R.new (local.get 0)))
        (func (export "rep") (param i32) (result i32) (local.get 0))
        (func (export "take") (param i32) (result i32) (call $R.rep (local.get 0)))
        (func (export "hidden") (result i32) (call $Hidden.new (i32.const 0))))
      (core instance $m (instantiate $M (with "" (instance
        (export "R.new" (func $R.new))
        (export "R.rep" (func $R.rep))
        (export "Hidden.new" (func $Hidden.new))))))
      (export $R' "r" (type $R))
      (func (export "[constructor]r") (param "rep" s32) (result (own $R'))
        (canon lift (core func $m "new")))
      (func (export "[method]r.rep") (param "self" (borrow $R')) (result s32)
        (canon lift (core func $m "rep")))
      (func (export "take") (param "r" (own $R')) (result s32)
        (canon lift (core func $m "take")))
      (func (export "hidden") (result (own $Hidden))
        (canon lift (core func $m "hidden")))
    )`)), { dropped }).exports;
}

// A borrow passes the rep to the implementing instance, and an own passes a
// handle to it.
{
  const { R, take } = instantiate();
  for (const rep of [0, -1, 0x7fffffff, -0x80000000]) {
    const r = new R(rep);
    assertEq(r.rep(), rep);
    assertEq(take(r), rep);
    assertErrorMessage(() => r.rep(), TypeError, /dropped or passed/);
    assertErrorMessage(() => take(r), TypeError, /dropped or passed/);
  }
}

// Disposing calls the destructor once, with the rep.
{
  const dropped = [];
  const { R } = instantiate({ dropped: rep => dropped.push(rep) });
  const r = new R(-5);
  assertEq(r[Symbol.dispose](), undefined);
  assertEq(r[Symbol.dispose](), undefined);
  assertEq(dropped.join(), "-5");
  assertErrorMessage(() => r.rep(), TypeError, /dropped or passed/);
}

// A trapping destructor locks down the instance.
{
  const { R } = instantiate({ dtorTraps: true });
  const a = new R(1);
  const b = new R(2);
  assertErrorMessage(() => a[Symbol.dispose](), WebAssembly.RuntimeError,
                     /unreachable/);
  assertErrorMessage(() => b.rep(), WebAssembly.RuntimeError, /locked down/);
  assertErrorMessage(() => b[Symbol.dispose](), WebAssembly.RuntimeError,
                     /locked down/);
}

// Instances of subclasses get their prototype.
{
  const { R } = instantiate();
  class S extends R {
    twice() { return this.rep() * 2; }
  }
  const s = new S(21);
  assertEq(Object.getPrototypeOf(s), S.prototype);
  assertEq(s.twice(), 42);
}

// Other classes' instances and other values are rejected.
{
  const a = instantiate();
  const b = instantiate();
  for (const value of [new b.R(1), {}, Object.create(a.R.prototype), 1]) {
    assertErrorMessage(() => a.take(value), TypeError,
                       /not a resource of the expected type/);
  }
}

// Resource types that aren't exported have no class.
{
  const { hidden } = instantiate();
  assertErrorMessage(() => hidden(), Error, /not exported/);
}

// Unreachable instances that own their resource are dropped, and ones whose
// resource was passed to the component or disposed are not dropped again.
{
  const dropped = [];
  const { R, take } = instantiate({ dropped: rep => dropped.push(rep) });
  (() => {
    new R(1);
    take(new R(2));
    new R(3)[Symbol.dispose]();
  })();
  assertEq(dropped.join(), "3");
  gc();
  drainJobQueue();
  assertEq(dropped.sort().join(), "1,3");
}
