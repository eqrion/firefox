// META: global=window,dedicatedworker,jsshell
// META: script=/wasm/component/jsapi/harness/component-helpers.js
// META: script=/wasm/component/jsapi/harness/assertions.js

// `read the component import`: the value must be a WebAssembly.Component
// whose type is a subtype of the imported component type.

promise_test(async () => {
  const inner = await compileComponent("nested/inner");
  const messages = [];
  const { run } = await componentExports("nested/component-import", {
    c: inner,
    log: message => messages.push(message),
  });
  assert_equals(run(), 42);
  assert_array_equals(messages, ["from inner"]);
}, "a Component satisfying the type is instantiated internally with host imports");

promise_test(async () => {
  const inner = await compileComponent("nested/inner");
  const imports = { c: inner, log: () => {} };
  const a = await componentExports("nested/component-import", imports);
  const b = await componentExports("nested/component-import", imports);
  assert_not_equals(a.run, b.run);
  assert_equals(a.run(), 42);
  assert_equals(b.run(), 42);
}, "the same Component can satisfy several instantiations");

for (const [value, description] of [
  [undefined, "undefined"],
  [null, "null"],
  [{}, "plain object"],
  [() => {}, "function"],
  [WebAssembly.Component.prototype, "Component.prototype"],
]) {
  test_instantiation_error(WebAssembly.LinkError, "nested/component-import",
                           { c: value, log: () => {} }, undefined,
                           `component import: ${description} is a LinkError`);
}

promise_test(async t => {
  const module = new WebAssembly.Module(emptyModuleBytes);
  await promise_rejects_js(t, WebAssembly.LinkError, (async () =>
    instantiateComponent("nested/component-import", { c: module, log: () => {} }))());
}, "component import: a WebAssembly.Module is a LinkError");

promise_test(async t => {
  const mismatch = await compileComponent("nested/inner-mismatch");
  await promise_rejects_js(t, WebAssembly.LinkError, (async () =>
    instantiateComponent("nested/component-import", { c: mismatch, log: () => {} }))());
}, "component import: a Component with a mismatching type is a LinkError");

promise_test(async t => {
  const empty = await compileComponent("realm/empty");
  await promise_rejects_js(t, WebAssembly.LinkError, (async () =>
    instantiateComponent("nested/component-import", { c: empty, log: () => {} }))());
}, "component import: a Component missing the exports is a LinkError");

promise_test(async () => {
  const inner = await compileComponent("nested/inner");
  const log = [];
  const imports = loggingProxy({ c: inner, log: () => {} }, log);
  await instantiateComponent("nested/component-import", imports);
  assert_array_equals(log, ["get c", "get log"]);
}, "component import: imports are read in declaration order");
