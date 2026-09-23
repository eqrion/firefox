// |jit-test| skip-if: !wasmComponentsEnabled()

// Resource handle tables and the canon resource.new, resource.rep, and
// resource.drop builtins.

// The exports of the first core instance of `instance` with an export `name`.
function coreExports(instance, name) {
  for (let i = 0; ; i++) {
    const exports = wasmComponentCoreInstanceExports(instance, i);
    if (name in exports) {
      return exports;
    }
  }
}

// $R has a destructor that calls `log` with the rep, or traps if `dtorTraps`.
// $S has no destructor.
function instantiate(log, dtorTraps = false) {
  const instance = new WebAssembly.ComponentInstance(
    new WebAssembly.Component(wasmTextToBinary(`(component
      (import "log" (func $log (param "rep" s32)))
      (core func $log' (canon lower (func $log)))
      (core module $Dtor
        (import "" "log" (func $log (param i32)))
        (func (export "dtor") (param i32)
          ${dtorTraps ? "unreachable" : "(call $log (local.get 0))"}))
      (core instance $dtor (instantiate $Dtor
        (with "" (instance (export "log" (func $log'))))))
      (type $R (resource (rep i32) (dtor (core func $dtor "dtor"))))
      (type $S (resource (rep i32)))
      (core func $R.new (canon resource.new $R))
      (core func $R.rep (canon resource.rep $R))
      (core func $R.drop (canon resource.drop $R))
      (core func $S.new (canon resource.new $S))
      (core func $S.rep (canon resource.rep $S))
      (core func $S.drop (canon resource.drop $S))
      (core module $Main
        (func (export "R.new") (import "" "R.new") (param i32) (result i32))
        (func (export "R.rep") (import "" "R.rep") (param i32) (result i32))
        (func (export "R.drop") (import "" "R.drop") (param i32))
        (func (export "S.new") (import "" "S.new") (param i32) (result i32))
        (func (export "S.rep") (import "" "S.rep") (param i32) (result i32))
        (func (export "S.drop") (import "" "S.drop") (param i32)))
      (core instance (instantiate $Main (with "" (instance
        (export "R.new" (func $R.new))
        (export "R.rep" (func $R.rep))
        (export "R.drop" (func $R.drop))
        (export "S.new" (func $S.new))
        (export "S.rep" (func $S.rep))
        (export "S.drop" (func $S.drop))))))
    )`)), { log });
  return coreExports(instance, "R.new");
}

// Handles are allocated in order from 1, and freed handles are reused last
// freed first.
{
  const dropped = [];
  const e = instantiate(rep => dropped.push(rep));
  assertEq(e["R.new"](10), 1);
  assertEq(e["R.new"](11), 2);
  assertEq(e["S.new"](12), 3);
  assertEq(e["R.rep"](2), 11);
  e["R.drop"](2);
  assertEq(dropped.join(), "11");
  assertEq(e["R.new"](13), 2);
  e["S.drop"](3);
  e["R.drop"](1);
  assertEq(dropped.join(), "11,10");
  assertEq(e["S.new"](14), 1);
  assertEq(e["R.new"](15), 3);
  assertEq(e["S.rep"](1), 14);
  assertEq(e["R.rep"](3), 15);
}

// Any i32 can be a rep.
{
  const e = instantiate(() => {});
  for (const rep of [0, -1, 0x7fffffff, -0x80000000]) {
    assertEq(e["R.rep"](e["R.new"](rep)), rep);
  }
}

// Handles must exist and have the right type.
{
  const e = instantiate(() => {});
  const r = e["R.new"](1);
  const s = e["S.new"](2);
  for (const bad of [0, 3, 100, -1]) {
    assertErrorMessage(() => e["R.rep"](bad), WebAssembly.RuntimeError,
                       /invalid resource handle/);
    assertErrorMessage(() => e["R.drop"](bad), WebAssembly.RuntimeError,
                       /invalid resource handle/);
  }
  assertErrorMessage(() => e["R.rep"](s), WebAssembly.RuntimeError,
                     /resource handle has the wrong type/);
  assertErrorMessage(() => e["S.drop"](r), WebAssembly.RuntimeError,
                     /resource handle has the wrong type/);
  e["R.drop"](r);
  assertErrorMessage(() => e["R.drop"](r), WebAssembly.RuntimeError,
                     /invalid resource handle/);
  assertErrorMessage(() => e["R.rep"](r), WebAssembly.RuntimeError,
                     /invalid resource handle/);
}

// A trapping destructor traps resource.drop, after the handle is removed.
{
  const e = instantiate(() => {}, /*dtorTraps=*/true);
  const r = e["R.new"](1);
  assertErrorMessage(() => e["R.drop"](r), WebAssembly.RuntimeError,
                     /unreachable/);
  assertErrorMessage(() => e["R.rep"](r), WebAssembly.RuntimeError,
                     /invalid resource handle/);
}

// The instance may not leave during realloc and post-return, so resource.new,
// resource.drop, and lowered imports trap there, which locks down the
// instance. resource.rep is allowed.
for (const [during, call, allowed] of [
  ["realloc", "(drop (call $R.new (i32.const 1)))", false],
  ["realloc",
   "(if (global.get $r) (then (drop (call $R.rep (global.get $r)))))", true],
  ["post-return", "(call $R.drop (global.get $r))", false],
  ["post-return", "(call $log (i32.const 1))", false],
]) {
  const { f, g } = new WebAssembly.ComponentInstance(
    new WebAssembly.Component(wasmTextToBinary(`(component
      (import "log" (func $log (param "rep" s32)))
      (type $R (resource (rep i32)))
      (core func $R.new (canon resource.new $R))
      (core func $R.rep (canon resource.rep $R))
      (core func $R.drop (canon resource.drop $R))
      (core func $log' (canon lower (func $log)))
      (core module $M
        (import "" "R.new" (func $R.new (param i32) (result i32)))
        (import "" "R.rep" (func $R.rep (param i32) (result i32)))
        (import "" "R.drop" (func $R.drop (param i32)))
        (import "" "log" (func $log (param i32)))
        (memory (export "mem") 1)
        (global $r (mut i32) (i32.const 0))
        (func (export "realloc") (param i32 i32 i32 i32) (result i32)
          ${during === "realloc" ? call : ""}
          (i32.const 16))
        (func (export "post-return") (param i32)
          ${during === "post-return" ? call : ""})
        (func (export "f") (param i32 i32) (result i32)
          (global.set $r (call $R.new (i32.const 1)))
          (i32.const 0))
        (func (export "g") (result i32) (i32.const 0)))
      (core instance $m (instantiate $M (with "" (instance
        (export "R.new" (func $R.new))
        (export "R.rep" (func $R.rep))
        (export "R.drop" (func $R.drop))
        (export "log" (func $log'))))))
      (func (export "f") (param "s" string) (result u32)
        (canon lift (core func $m "f")
          (memory (core memory $m "mem")) (realloc (core func $m "realloc"))
          (post-return (core func $m "post-return"))))
      (func (export "g") (result u32)
        (canon lift (core func $m "g")))
    )`)), { log: () => {} }).exports;
  if (allowed) {
    assertEq(f(""), 0);
    assertEq(f("x"), 0);
  } else {
    assertErrorMessage(() => f(during === "realloc" ? "x" : ""),
                       WebAssembly.RuntimeError,
                       /cannot be left during realloc or post-return/);
    assertErrorMessage(() => g(), WebAssembly.RuntimeError, /locked down/);
  }
}
