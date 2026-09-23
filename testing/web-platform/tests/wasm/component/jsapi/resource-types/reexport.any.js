// META: global=window,dedicatedworker,jsshell
// META: script=/wasm/component/jsapi/harness/component-helpers.js
// META: script=/wasm/component/jsapi/harness/assertions.js

// Re-exporting an imported resource type. The public form
// `(export "x" (type (eq $r)))` is rejected by `create guest resource
// classes` with a TypeError. The private form, a fresh abstract export of an
// imported type, cannot currently be expressed with wasm-tools and is not
// covered here.

class C {}

test_instantiation_error(TypeError, "resources/reexport-public", { R: C, make: () => new C() }, undefined,
                         "publicly re-exporting an imported resource type is a TypeError");

promise_test(async t => {
  let called = false;
  const imports = { R: C, make: () => { called = true; return new C(); } };
  await promise_rejects_js(t, TypeError,
                           WebAssembly.instantiate(await componentBytes("resources/reexport-public"), imports));
  assert_false(called, "no export was invoked");
}, "the re-export TypeError happens during instantiation, before any export is callable");
