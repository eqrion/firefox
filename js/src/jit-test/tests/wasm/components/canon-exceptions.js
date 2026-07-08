// |jit-test| skip-if: !wasmComponentsEnabled(); local-include:canon-instantiate-harness.js

// Exception propagation through the canon lift/lower pipelines, through
// real instantiation. A lift's "core callee" is always real core wasm, so
// making it throw means wiring it to call a throwing JS import - the
// canon-lower half of the round trip greeter.js also exercises.

// Builds a component exporting "f" (canon lift of a core func that just
// calls through to the imported "callee"), and if `withPostReturn`, a
// second import "postReturn" wired as the lift's post-return hook.
function liftOfThrowingImport(withPostReturn) {
  const postReturnImport = withPostReturn
    ? `(import "post-return" (func $postReturn (param "v" s32)))
       (core func $postReturnLowered (canon lower (func $postReturn)))`
    : "";
  const postReturnExport = withPostReturn ? `(export "postReturn" (func $postReturnLowered))` : "";
  const postReturnClause = withPostReturn ? ` (post-return (func $postReturnCore))` : "";
  const postReturnAlias = withPostReturn
    ? `(alias core export $implInst "postReturn" (core func $postReturnCore))`
    : "";
  const importDecl = withPostReturn
    ? `(import "env" "postReturn" (func $postReturn (param i32)))`
    : "";

  const component = new WebAssembly.Component(wasmTextToBinary(`
    (component
      (type $t0 (func (param "v" s32) (result s32)))
      (import "callee" (func $callee (type $t0)))
      (core func $calleeLowered (canon lower (func $callee)))
      ${postReturnImport}
      (core instance $env
        (export "callee" (func $calleeLowered))
        ${postReturnExport})

      (core module $impl
        (import "env" "callee" (func $callee (param i32) (result i32)))
        ${importDecl}
        ${CANON_HARNESS_MEMORY_REALLOC}
        (func (export "f") (param $v i32) (result i32) (call $callee (local.get $v)))
        ${withPostReturn ? `(func (export "postReturn") (param $v i32) (call $postReturn (local.get $v)))` : ""}
      )
      (core instance $implInst (instantiate $impl (with "env" (instance $env))))
      (alias core export $implInst "f" (core func $implF))
      (alias core export $implInst "mem" (core memory $mem))
      (alias core export $implInst "realloc" (core func $realloc))
      ${postReturnAlias}

      (func $lifted (type $t0)
        (canon lift (core func $implF) (memory $mem) (realloc $realloc)${postReturnClause}))
      (export "f" (func $lifted))
    )
  `));
  return component;
}

{
  const throwingCallee = () => {
    throw new Error("boom-lift");
  };
  const instance = new WebAssembly.ComponentInstance(
    liftOfThrowingImport(false), { callee: throwingCallee });
  let threw = false;
  try {
    instance.exports.f(1);
  } catch (e) {
    threw = true;
    assertEq(e.message, "boom-lift");
  }
  assertEq(threw, true);
}

{
  const throwingPostReturn = () => {
    throw new Error("boom-postreturn");
  };
  const instance = new WebAssembly.ComponentInstance(
    liftOfThrowingImport(true), { callee: (v) => v, postReturn: throwingPostReturn });
  let threw = false;
  try {
    instance.exports.f(42);
  } catch (e) {
    threw = true;
    assertEq(e.message, "boom-postreturn");
  }
  assertEq(threw, true);
}

{
  const throwingFn = () => {
    throw new Error("boom-lower");
  };
  const { caller } = lowerThrough(
    `(param "v" s32) (result s32)`,
    throwingFn,
    `(import "env" "f" (func $f (param i32) (result i32)))
     (export "f" (func $f))`);
  let threw = false;
  try {
    caller.f(1);
  } catch (e) {
    threw = true;
    assertEq(e.message, "boom-lower");
  }
  assertEq(threw, true);
}

{
  const throwingFn = () => {
    throw new Error("boom-import");
  };
  const { caller } = lowerThrough(
    `(param "v" s32) (result s32)`,
    throwingFn,
    `(import "env" "f" (func $imp (param i32) (result i32)))
     (func (export "callit") (param $a i32) (result i32)
       (call $imp (local.get $a)))`);
  let threw = false;
  try {
    caller.callit(1);
  } catch (e) {
    threw = true;
    assertEq(e.message, "boom-import");
  }
  assertEq(threw, true);
}
