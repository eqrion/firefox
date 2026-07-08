// Shared boilerplate for driving canon lift/lower through real component
// instantiation (see greeter.js for the base pattern), used by the migrated
// canon-lift-*/canon-lower-* tests. Each test still supplies the exact core
// wasm module implementing/consuming the flattened ABI it wants to exercise
// (just like the pre-instantiation tests did); this harness only wires up
// the standard memory/realloc plumbing and the canon lift/lower/export/
// import declarations around it.

// Interpolate this INSIDE a test's own core module body (not as a separate
// module) - canon lift/lower's memory/realloc option needs to resolve to
// the SAME memory the core func's own load/store instructions use, so they
// can't live in a separate instance.
const CANON_HARNESS_MEMORY_REALLOC = `
    (memory (export "mem") 1)
    (global $top (mut i32) (i32.const 4096))
    (func (export "realloc")
          (param $origPtr i32) (param $origSize i32)
          (param $align i32) (param $newSize i32) (result i32)
      (local $ptr i32)
      (local $mask i32)
      (local.set $mask (i32.sub (local.get $align) (i32.const 1)))
      (global.set $top
        (i32.and
          (i32.add (global.get $top) (local.get $mask))
          (i32.xor (local.get $mask) (i32.const -1))))
      (local.set $ptr (global.get $top))
      (global.set $top (i32.add (global.get $top) (local.get $newSize)))
      (local.get $ptr))
`;

// Builds and instantiates a component that `canon lift`s a core func into a
// JS-callable export "f":
//   - `funcTypeText` is the component-level `(func ...)` type body, e.g.
//     `(param "v" s32) (result s32)`. If `resourceImport` is set, it may
//     reference an imported resource type as `$T` (e.g. `(own $T)`).
//   - `implModuleText` is a core module WAT fragment that, if the func type
//     needs memory (strings/lists/spilled composites), should interpolate
//     CANON_HARNESS_MEMORY_REALLOC alongside its own funcs, and exports a
//     func "f" (and, if `postReturn` is set, a func "cabi_post") with the
//     raw flattened core signature `funcTypeText` implies.
//   - `imports`, if given, is passed through to the ComponentInstance
//     constructor - e.g. `{ R: someWebIDLInterface }` to bind `$T`'s
//     interface (see canon-resource-interfaces.js). Note the PascalCase
//     key: resource/type import names map to PascalCase, not camelCase
//     (see name-mangling-resource-methods.js).
// Returns the lifted export.
function liftThrough(funcTypeText, implModuleText,
                     { postReturn = false, resourceImport = false, imports = {} } = {}) {
  const postReturnClause = postReturn
    ? ` (post-return (func $implF_postReturn))`
    : "";
  const postReturnAlias = postReturn
    ? `(alias core export $implInst "cabi_post" (core func $implF_postReturn))`
    : "";
  const resourceImportDecl = resourceImport ? `(import "r" (type $T (sub resource)))` : "";
  const component = new WebAssembly.Component(wasmTextToBinary(`
    (component
      ${resourceImportDecl}
      (core module $impl ${implModuleText})
      (core instance $implInst (instantiate $impl))
      (alias core export $implInst "f" (core func $implF))
      (alias core export $implInst "mem" (core memory $mem))
      (alias core export $implInst "realloc" (core func $realloc))
      ${postReturnAlias}

      (type $t (func ${funcTypeText}))
      (func $lifted (type $t)
        (canon lift (core func $implF) (memory $mem) (realloc $realloc)${postReturnClause}))
      (export "f" (func $lifted))
    )
  `));
  const instance = new WebAssembly.ComponentInstance(component, imports);
  return instance.exports.f;
}

// Builds and instantiates a component that imports a JS function "f"
// (component-level type `funcTypeText`), `canon lower`s it into a core
// import for `callerModuleText`, and instantiates that as a real core
// instance so its exports are reachable with the raw flattened core calling
// convention (mirroring what the removed ComponentCanonLower built-in
// returned directly):
//   - `callerModuleText` must `(import "env" "f" ...)` with the flattened
//     core signature `funcTypeText` implies, and export whatever the test
//     wants to observe/call - a direct re-export (no wrapper body needed) is
//     enough when the test just wants to drive the lowered func's raw
//     calling convention.
// Returns `{ caller, memory, realloc }`: `caller` is `callerModuleText`'s
// exports; `memory`/`realloc` back the ABI, for tests that build their own
// pointers for string/list args/results from JS (mirroring the removed
// canon-lower-harness.js's bytesToPtr/utf8Bytes helpers).
function lowerThrough(funcTypeText, jsCallee, callerModuleText) {
  const component = new WebAssembly.Component(wasmTextToBinary(`
    (component
      (core module $libc ${CANON_HARNESS_MEMORY_REALLOC})
      (core instance $libcInst (instantiate $libc))
      (alias core export $libcInst "mem" (core memory $mem))
      (alias core export $libcInst "realloc" (core func $realloc))

      (type $t (func ${funcTypeText}))
      (import "f" (func $f (type $t)))
      (core func $lowered (canon lower (func $f) (memory $mem) (realloc $realloc)))
      (core instance $env (export "f" (func $lowered)))

      (core module $caller ${callerModuleText})
      (core instance $callerInst (instantiate $caller (with "env" (instance $env))))
    )
  `));
  const instance = new WebAssembly.ComponentInstance(component, { f: jsCallee });
  // Core instance indices, in declaration order above: $libcInst=0, the
  // inline-export $env=1, $callerInst=2.
  const libcInstance = wasmComponentCoreInstance(instance, 0);
  const callerInstance = wasmComponentCoreInstance(instance, 2);
  return {
    caller: callerInstance.exports,
    memory: libcInstance.exports.mem,
    realloc: libcInstance.exports.realloc,
  };
}
