// META: global=window,dedicatedworker,jsshell
// META: script=/wasm/component/jsapi/harness/component-helpers.js
// META: script=/wasm/component/jsapi/harness/assertions.js

// Enabled builtins are a property of the Component, not of the instantiation
// call. component/global-echo imports `component-test-echo` from
// `wasm:js/global`.

const FIXTURE = "component/global-echo";

promise_setup(async () => {
  globalThis.componentTestEcho = x => x + 100;
});

promise_test(async () => {
  const component = await compileComponent(FIXTURE, { builtins: ["js/global"] });
  const instance = new WebAssembly.ComponentInstance(component);
  assert_equals(instance.exports.roundtrip(1), 101);
}, "builtins on the Component: new ComponentInstance resolves the builtin");

promise_test(async () => {
  const component = await compileComponent(FIXTURE, { builtins: ["js/global"] });
  const instance = await WebAssembly.instantiate(component);
  assert_equals(instance.exports.roundtrip(2), 102);
}, "builtins on the Component: WebAssembly.instantiate(Component) resolves the builtin");

promise_test(async () => {
  const component = await WebAssembly.compile(await componentBytes(FIXTURE), { builtins: ["js/global"] });
  const instance = new WebAssembly.ComponentInstance(component);
  assert_equals(instance.exports.roundtrip(3), 103);
}, "builtins on the Component: WebAssembly.compile options carry over");

promise_test(async () => {
  const { instance } = await WebAssembly.instantiate(await componentBytes(FIXTURE), undefined,
                                                     { builtins: ["js/global"] });
  assert_equals(instance.exports.roundtrip(4), 104);
}, "builtins on the Component: WebAssembly.instantiate(bytes) options apply");

promise_test(async () => {
  const component = await compileComponent(FIXTURE);
  assert_throws_js(TypeError, () => new WebAssembly.ComponentInstance(component));
  assert_throws_js(WebAssembly.LinkError, () => new WebAssembly.ComponentInstance(component, {}));
}, "builtins not on the Component: new ComponentInstance does not resolve the builtin");

promise_test(async t => {
  const component = await compileComponent(FIXTURE);
  await promise_rejects_js(t, TypeError, WebAssembly.instantiate(component));
  await promise_rejects_js(t, WebAssembly.LinkError, WebAssembly.instantiate(component, {}));
}, "builtins not on the Component: WebAssembly.instantiate(Component) does not resolve the builtin");

promise_test(async () => {
  const component = await compileComponent(FIXTURE);
  const instance = new WebAssembly.ComponentInstance(component, { "wasm:js/global": globalThis });
  assert_equals(instance.exports.roundtrip(5), 105);
}, "builtins not on the Component: the import can still be satisfied explicitly");

promise_test(async () => {
  const component = await compileComponent(FIXTURE, { builtins: ["js/global"] });
  const a = new WebAssembly.ComponentInstance(component);
  const b = new WebAssembly.ComponentInstance(component, {});
  const c = await WebAssembly.instantiate(component);
  assert_equals(a.exports.roundtrip(1), 101);
  assert_equals(b.exports.roundtrip(1), 101);
  assert_equals(c.exports.roundtrip(1), 101);
}, "builtins on the Component: every instantiation of the same Component sees them");

promise_test(async () => {
  const component = await compileComponent(FIXTURE, { builtins: ["js/global"] });
  const imports = { "wasm:js/global": { componentTestEcho: x => x + 900 } };
  const instance = new WebAssembly.ComponentInstance(component, imports);
  assert_equals(instance.exports.roundtrip(1), 101, "the builtin wins over the imports object");
}, "builtins on the Component: the imports object cannot override a builtin");
