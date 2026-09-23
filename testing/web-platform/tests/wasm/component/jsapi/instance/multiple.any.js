// META: global=window,dedicatedworker,jsshell
// META: script=/wasm/component/jsapi/harness/component-helpers.js
// META: script=/wasm/component/jsapi/harness/assertions.js

// Instances of one Component are independent: separate state, separate
// exports objects, separate guest resource classes.

const FIXTURE = "instance/multiple";

promise_test(async () => {
  const component = await compileComponent(FIXTURE);
  const a = new WebAssembly.ComponentInstance(component);
  const b = new WebAssembly.ComponentInstance(component);
  assert_equals(a.exports.increment(), 1);
  assert_equals(a.exports.increment(), 2);
  assert_equals(b.exports.increment(), 1);
  assert_equals(a.exports.increment(), 3);
}, "multiple instances: state is per instance");

promise_test(async () => {
  const component = await compileComponent(FIXTURE);
  const a = new WebAssembly.ComponentInstance(component);
  const b = new WebAssembly.ComponentInstance(component);
  assert_not_equals(a, b);
  assert_not_equals(a.exports, b.exports);
  assert_not_equals(a.exports.increment, b.exports.increment);
  assert_not_equals(a.exports.Counter, b.exports.Counter);
  assert_not_equals(a.exports.Counter.prototype, b.exports.Counter.prototype);
  assert_not_equals(a.exports.Counter.prototype.get, b.exports.Counter.prototype.get);
  assert_array_equals(Reflect.ownKeys(a.exports), Reflect.ownKeys(b.exports));
}, "multiple instances: exports objects, functions and classes are distinct");

promise_test(async () => {
  const component = await compileComponent(FIXTURE);
  const a = new WebAssembly.ComponentInstance(component);
  const b = new WebAssembly.ComponentInstance(component);
  const counter = new a.exports.Counter(5);
  assert_equals(counter.get(), 5);
  assert_true(counter instanceof a.exports.Counter);
  assert_false(counter instanceof b.exports.Counter);
  assert_throws_js(TypeError, () => b.exports.Counter.prototype.get.call(counter));
  assert_equals(a.exports.Counter.prototype.get.call(counter), 5, "the instance still works with its own class");
}, "multiple instances: a resource instance is rejected by another instance's class");

promise_test(async () => {
  const bytes = await componentBytes(FIXTURE);
  const a = new WebAssembly.ComponentInstance(new WebAssembly.Component(bytes));
  const b = new WebAssembly.ComponentInstance(new WebAssembly.Component(bytes));
  assert_equals(a.exports.increment(), 1);
  assert_equals(b.exports.increment(), 1);
  assert_not_equals(a.exports.Counter, b.exports.Counter);
  assert_throws_js(TypeError, () => b.exports.Counter.prototype.get.call(new a.exports.Counter(1)));
}, "multiple instances: two Components from the same bytes are also independent");

promise_test(async () => {
  const component = await compileComponent(FIXTURE);
  const instances = [];
  for (const [, entry] of instantiationEntryPoints) {
    instances.push(await entry(await componentBytes(FIXTURE)));
  }
  instances.push(new WebAssembly.ComponentInstance(component));
  for (const instance of instances) {
    assert_equals(instance.exports.increment(), 1);
  }
}, "multiple instances: every entry point produces a fresh instance");
