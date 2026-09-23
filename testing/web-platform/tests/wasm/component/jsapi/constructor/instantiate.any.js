// META: global=window,dedicatedworker,jsshell
// META: script=/wasm/component/jsapi/harness/component-helpers.js
// META: script=/wasm/component/jsapi/harness/assertions.js

// WebAssembly.instantiate dispatches on the `layer` field of the binary header
// and accepts a Component in place of a Module.

function assert_WebAssemblyInstantiatedComponentSource(result, expectedKeys) {
  assert_equals(Object.getPrototypeOf(result), Object.prototype, "result prototype");
  assert_true(Object.isExtensible(result), "result extensible");
  assert_array_equals(Object.keys(result).sort(), ["component", "instance"], "result keys");
  for (const key of ["component", "instance"]) {
    const desc = Object.getOwnPropertyDescriptor(result, key);
    assert_true(desc.writable, `${key}: writable`);
    assert_true(desc.enumerable, `${key}: enumerable`);
    assert_true(desc.configurable, `${key}: configurable`);
  }
  assert_equals(Object.getPrototypeOf(result.component), WebAssembly.Component.prototype,
                "component prototype");
  assert_ComponentInstance(result.instance, expectedKeys);
}

promise_test(async () => {
  const result = await WebAssembly.instantiate(await componentBytes("constructor/no-imports"));
  assert_WebAssemblyInstantiatedComponentSource(result, ["answer"]);
  assert_equals(result.instance.exports.answer(), 42);
}, "instantiate(bytes): component bytes resolve to { component, instance }");

promise_test(async () => {
  const result = await WebAssembly.instantiate(await componentBytes("constructor/no-imports"), {});
  assert_WebAssemblyInstantiatedComponentSource(result, ["answer"]);
}, "instantiate(bytes, {}): empty imports object is accepted for a component without imports");

promise_test(async () => {
  const result = await WebAssembly.instantiate(await componentBytes("constructor/echo"), { echo: x => x + 1 });
  assert_WebAssemblyInstantiatedComponentSource(result, ["roundtrip"]);
  assert_equals(result.instance.exports.roundtrip(1), 2);
}, "instantiate(bytes, imports): imports are used");

promise_test(async () => {
  const result = await WebAssembly.instantiate(emptyModuleBytes);
  assert_array_equals(Object.keys(result).sort(), ["instance", "module"]);
  assert_equals(Object.getPrototypeOf(result.module), WebAssembly.Module.prototype);
  assert_equals(Object.getPrototypeOf(result.instance), WebAssembly.Instance.prototype);
}, "instantiate(bytes): core module bytes still resolve to { module, instance }");

promise_test(async () => {
  const component = await compileComponent("constructor/no-imports");
  const instance = await WebAssembly.instantiate(component);
  assert_ComponentInstance(instance, ["answer"]);
  assert_equals(instance.exports.answer(), 42);
}, "instantiate(Component): resolves to a ComponentInstance");

promise_test(async () => {
  const component = await compileComponent("constructor/echo");
  const instance = await WebAssembly.instantiate(component, { echo: x => x * 2 });
  assert_ComponentInstance(instance, ["roundtrip"]);
  assert_equals(instance.exports.roundtrip(4), 8);
}, "instantiate(Component, imports): imports are used");

promise_test(async () => {
  const component = await compileComponent("constructor/no-imports");
  const a = await WebAssembly.instantiate(component);
  const b = await WebAssembly.instantiate(component);
  assert_not_equals(a, b);
  assert_not_equals(a.exports, b.exports);
}, "instantiate(Component): each call creates a fresh instance");

promise_test(async t => {
  await promise_rejects_js(t, WebAssembly.CompileError,
                           WebAssembly.instantiate(new Uint8Array([...emptyComponentBytes, 0xff])));
}, "instantiate(bytes): a malformed component rejects with CompileError");

promise_test(async t => {
  for (const argument of [undefined, null, true, "", Symbol(), 1, {}, [],
                          WebAssembly.Component, WebAssembly.Component.prototype]) {
    await promise_rejects_js(t, TypeError, WebAssembly.instantiate(argument),
                             `instantiate(${format_value(argument)})`);
  }
}, "instantiate: non-BufferSource, non-Module, non-Component arguments reject with TypeError");

promise_test(async t => {
  const bytes = await componentBytes("constructor/echo");
  let promise = WebAssembly.instantiate(bytes);
  assert_true(promise instanceof Promise, "missing imports: returns a promise");
  await promise_rejects_js(t, TypeError, promise, "missing imports");

  promise = WebAssembly.instantiate(bytes, { echo: 1 });
  assert_true(promise instanceof Promise, "non-callable import: returns a promise");
  await promise_rejects_js(t, WebAssembly.LinkError, promise, "non-callable import");

  promise = WebAssembly.instantiate(bytes, 42);
  assert_true(promise instanceof Promise, "non-object imports: returns a promise");
  await promise_rejects_js(t, TypeError, promise, "non-object imports");
}, "instantiate(bytes): bad imports reject instead of throwing synchronously");

promise_test(async t => {
  const component = await compileComponent("constructor/echo");
  let promise = WebAssembly.instantiate(component);
  assert_true(promise instanceof Promise, "missing imports: returns a promise");
  await promise_rejects_js(t, TypeError, promise, "missing imports");

  promise = WebAssembly.instantiate(component, { echo: null });
  assert_true(promise instanceof Promise, "non-callable import: returns a promise");
  await promise_rejects_js(t, WebAssembly.LinkError, promise, "non-callable import");
}, "instantiate(Component): bad imports reject instead of throwing synchronously");

promise_test(async t => {
  const bytes = await componentBytes("constructor/no-imports");
  await promise_rejects_js(t, TypeError, WebAssembly.instantiate(bytes, undefined, { builtins: 1 }));
  await promise_rejects_js(t, TypeError, WebAssembly.instantiate(bytes, undefined, 1));
  const result = await WebAssembly.instantiate(bytes, undefined, { builtins: ["js/global"] });
  assert_WebAssemblyInstantiatedComponentSource(result, ["answer"]);
}, "instantiate(bytes, imports, options): compile options are parsed");

promise_test(async () => {
  const order = [];
  const bytes = await componentBytes("constructor/echo");
  const imports = { get echo() { order.push("echo"); return x => x; } };
  const promise = WebAssembly.instantiate(bytes, imports);
  order.push("returned");
  await promise;
  assert_array_equals(order, ["returned", "echo"]);
}, "instantiate(bytes): imports are read asynchronously, after the call returns");

promise_test(async () => {
  const bytes = await componentBytesCopy("constructor/no-imports");
  const promise = WebAssembly.instantiate(bytes);
  bytes.fill(0);
  const { instance } = await promise;
  assert_equals(instance.exports.answer(), 42);
}, "instantiate(bytes): bytes are copied before compilation");
