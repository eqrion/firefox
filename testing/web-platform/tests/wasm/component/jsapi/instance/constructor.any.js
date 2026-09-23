// META: global=window,dedicatedworker,jsshell
// META: script=/wasm/component/jsapi/harness/component-helpers.js
// META: script=/wasm/component/jsapi/harness/assertions.js

// new WebAssembly.ComponentInstance(component, importsObject) instantiates
// synchronously.

test(() => {
  assert_throws_js(TypeError, () => new WebAssembly.ComponentInstance());
}, "new ComponentInstance: no arguments throws TypeError");

test(() => {
  const invalidArguments = [
    undefined, null, true, "", Symbol(), 1, {}, [],
    WebAssembly.Component, WebAssembly.Component.prototype,
    new WebAssembly.Module(emptyModuleBytes),
    new WebAssembly.Instance(new WebAssembly.Module(emptyModuleBytes)),
  ];
  for (const argument of invalidArguments) {
    assert_throws_js(TypeError, () => new WebAssembly.ComponentInstance(argument),
                     `new ComponentInstance(${format_value(argument)})`);
  }
}, "new ComponentInstance: non-Component arguments throw TypeError");

promise_test(async () => {
  const component = await compileComponent("instance/no-imports");
  const instance = new WebAssembly.ComponentInstance(component);
  assert_ComponentInstance(instance, ["answer"]);
  assert_equals(Object.prototype.toString.call(instance), "[object WebAssembly.ComponentInstance]");
  assert_equals(instance.exports.answer(), 42);
}, "new ComponentInstance: component without imports, no imports argument");

promise_test(async () => {
  const component = await compileComponent("instance/no-imports");
  for (const imports of [undefined, {}, [], () => {}, new Proxy({}, {}), Object.create(null)]) {
    const instance = new WebAssembly.ComponentInstance(component, imports);
    assert_equals(instance.exports.answer(), 42, `imports ${format_value(imports)}`);
  }
}, "new ComponentInstance: component without imports accepts undefined or any object");

promise_test(async () => {
  const component = await compileComponent("instance/no-imports");
  for (const imports of [null, 42, "", true, Symbol()]) {
    assert_throws_js(TypeError, () => new WebAssembly.ComponentInstance(component, imports),
                     `imports ${format_value(imports)}`);
  }
}, "new ComponentInstance: non-object imports argument throws TypeError even without imports");

promise_test(async () => {
  const component = await compileComponent("instance/no-imports");
  const log = [];
  new WebAssembly.ComponentInstance(component, loggingProxy({}, log));
  assert_array_equals(log, []);
}, "new ComponentInstance: component without imports never reads the imports object");

promise_test(async () => {
  const component = await compileComponent("instance/echo");
  assert_throws_js(TypeError, () => new WebAssembly.ComponentInstance(component));
  assert_throws_js(TypeError, () => new WebAssembly.ComponentInstance(component, undefined));
}, "new ComponentInstance: component with imports and no imports argument throws TypeError");

promise_test(async () => {
  const component = await compileComponent("instance/echo");
  assert_throws_js(WebAssembly.LinkError, () => new WebAssembly.ComponentInstance(component, {}));
}, "new ComponentInstance: a missing function import throws LinkError");

promise_test(async () => {
  const component = await compileComponent("instance/echo");
  const instance = new WebAssembly.ComponentInstance(component, { echo: x => x + 1 });
  assert_ComponentInstance(instance, ["roundtrip"]);
  assert_equals(instance.exports.roundtrip(1), 2);
}, "new ComponentInstance: component with imports");

promise_test(async () => {
  const component = await compileComponent("instance/echo");
  const instance = new WebAssembly.ComponentInstance(component, { echo: x => x });
  assert_false(instance instanceof Promise);
  assert_equals(Object.getPrototypeOf(instance), WebAssembly.ComponentInstance.prototype);
}, "new ComponentInstance: returns the instance synchronously");

promise_test(async () => {
  const component = await compileComponent("instance/no-imports");
  const instance = Reflect.construct(WebAssembly.ComponentInstance, [component], WebAssembly.Instance);
  assert_equals(Object.getPrototypeOf(instance), WebAssembly.Instance.prototype);
  const getter = Object.getOwnPropertyDescriptor(WebAssembly.ComponentInstance.prototype, "exports").get;
  assert_equals(getter.call(instance).answer(), 42);
}, "new ComponentInstance: newTarget.prototype is used");

promise_test(async () => {
  const component = await compileComponent("instance/no-imports");
  const instance = new WebAssembly.ComponentInstance(component);
  const exports = instance.exports;
  instance.exports = 1;
  assert_equals(instance.exports, exports);
}, "new ComponentInstance: exports is read-only");
