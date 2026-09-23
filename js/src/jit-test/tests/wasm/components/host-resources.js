// |jit-test| skip-if: !wasmComponentsEnabled()

// Resources of imported resource types, whose values are JS objects. The
// JS-API behavior is also covered by the WPT tests in wasm/component/jsapi.

// The component keeps one owned handle of $r, and forwards borrows to the
// `[method]r.get` import. `lend(r)` drops the handle it was lent unless
// `keep` is set.
function instantiate(R, { keep = false } = {}) {
  return new WebAssembly.ComponentInstance(
    new WebAssembly.Component(wasmTextToBinary(`(component
      (import "r" (type $r (sub resource)))
      (import "[method]r.get" (func $get (param "self" (borrow $r)) (result u32)))
      (core func $get' (canon lower (func $get)))
      (core func $drop (canon resource.drop $r))
      (core module $M
        (import "" "get" (func $get (param i32) (result i32)))
        (import "" "drop" (func $drop (param i32)))
        (global $kept (mut i32) (i32.const 0))
        (func (export "keep") (param i32) (result i32)
          (global.set $kept (local.get 0))
          (local.get 0))
        (func (export "give-back") (result i32) (global.get $kept))
        (func (export "drop-kept") (call $drop (global.get $kept)))
        (func (export "get-kept") (result i32) (call $get (global.get $kept)))
        (func (export "lend") (param i32) (result i32)
          (local $result i32)
          (local.set $result (call $get (local.get 0)))
          ${keep ? "" : "(call $drop (local.get 0))"}
          (local.get $result)))
      (core instance $m (instantiate $M (with "" (instance
        (export "get" (func $get'))
        (export "drop" (func $drop))))))
      (func (export "keep") (param "r" (own $r)) (result u32)
        (canon lift (core func $m "keep")))
      (func (export "give-back") (result (own $r))
        (canon lift (core func $m "give-back")))
      (func (export "drop-kept") (canon lift (core func $m "drop-kept")))
      (func (export "get-kept") (result u32) (canon lift (core func $m "get-kept")))
      (func (export "lend") (param "r" (borrow $r)) (result u32)
        (canon lift (core func $m "lend")))
    )`)), { R }).exports;
}

class R {
  constructor(value) { this.value = value; }
  get() { return this.value; }
}

// An owned handle holds the JS object, which is lifted back as itself.
{
  const { keep, giveBack, getKept } = instantiate(R);
  const r = new R(5);
  assertEq(keep(r), 1);
  assertEq(getKept(), 5);
  assertEq(giveBack(), r);
  assertErrorMessage(() => giveBack(), WebAssembly.RuntimeError,
                     /invalid resource handle/);
}

// A borrowed handle can be lent on to an import, and is dropped by the guest.
{
  const { lend } = instantiate(R);
  const r = new R(7);
  assertEq(lend(r), 7);
  assertEq(lend(r), 7);
}

// A borrowed handle that isn't dropped traps when the call returns.
{
  const { lend } = instantiate(R, { keep: true });
  assertErrorMessage(() => lend(new R(1)), WebAssembly.RuntimeError,
                     /borrowed resource handles were not dropped/);
  assertErrorMessage(() => lend(new R(1)), WebAssembly.RuntimeError,
                     /locked down/);
}

// Dropping an owned handle of an imported type releases the JS object.
{
  const { keep, dropKept, giveBack } = instantiate(R);
  keep(new R(1));
  dropKept();
  assertErrorMessage(() => giveBack(), WebAssembly.RuntimeError,
                     /invalid resource handle/);
}

// Values are brand checked with instanceof, before entering the guest.
{
  const { keep, lend } = instantiate(R);
  for (const value of [{}, null, 1, new (class Other {})()]) {
    assertErrorMessage(() => keep(value), TypeError,
                       /not a resource of the expected type/);
    assertErrorMessage(() => lend(value), TypeError,
                       /not a resource of the expected type/);
  }
  assertEq(keep(Object.create(R.prototype)), 1);
}
