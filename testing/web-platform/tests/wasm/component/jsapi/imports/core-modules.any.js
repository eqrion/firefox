// META: global=window,dedicatedworker,jsshell
// META: script=/wasm/component/jsapi/harness/component-helpers.js
// META: script=/wasm/component/jsapi/harness/assertions.js

async function coreModule(name) {
  return new WebAssembly.Module(await componentBytes(`imports/${name}`));
}

promise_test(async () => {
  const m = await coreModule("core-modules-add");
  const { add } = await componentExports("imports/core-modules", { m });
  assert_equals(add(2, 3), 5);
}, "a WebAssembly.Module with a matching type satisfies a core module import");

promise_test(async () => {
  const m = await coreModule("core-modules-extra");
  const { add } = await componentExports("imports/core-modules", { m });
  assert_equals(add(2, 3), 5);
}, "a module with extra exports is a subtype and links");

promise_test(async t => {
  const component = await compileComponent("imports/core-modules");
  for (const [name, description] of [
    ["core-modules-wrong-sig", "wrong signature"],
    ["core-modules-missing", "missing export"],
  ]) {
    const m = await coreModule(name);
    assert_throws_js(WebAssembly.LinkError, () => new WebAssembly.ComponentInstance(component, { m }), description);
    await promise_rejects_js(t, WebAssembly.LinkError, WebAssembly.instantiate(component, { m }), description);
  }
}, "a module whose type does not match is a LinkError");

for (const [value, description] of [
  [undefined, "undefined"], [null, "null"], [{}, "plain object"], [() => {}, "function"],
  [WebAssembly.Module.prototype, "Module.prototype"], [1, "number"],
]) {
  test_instantiation_error(WebAssembly.LinkError, "imports/core-modules", { m: value }, undefined,
                           `a non-Module value (${description}) for a core module import is a LinkError`);
}

promise_test(async t => {
  const component = await compileComponent("imports/core-modules");
  const instance = new WebAssembly.Instance(await coreModule("core-modules-add"));
  assert_throws_js(WebAssembly.LinkError, () => new WebAssembly.ComponentInstance(component, { m: instance }));
  await promise_rejects_js(t, WebAssembly.LinkError, WebAssembly.instantiate(component, { m: instance }));
}, "a WebAssembly.Instance does not satisfy a core module import");

promise_test(async t => {
  const componentAsModule = await compileComponent("imports/core-modules-add").catch(() => null);
  assert_equals(componentAsModule, null, "core module bytes do not compile as a component");
  const component = await compileComponent("imports/core-modules");
  await promise_rejects_js(t, WebAssembly.LinkError, WebAssembly.instantiate(component, { m: component }));
}, "a WebAssembly.Component does not satisfy a core module import");

promise_test(async () => {
  const m = await coreModule("core-modules-add");
  const a = await componentExports("imports/core-modules", { m });
  const b = await componentExports("imports/core-modules", { m });
  assert_equals(a.add(1, 1), 2);
  assert_equals(b.add(2, 2), 4);
}, "one Module may satisfy the import of several component instances");
