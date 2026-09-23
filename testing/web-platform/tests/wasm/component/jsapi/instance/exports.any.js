// META: global=window,dedicatedworker,jsshell
// META: script=/wasm/component/jsapi/harness/component-helpers.js
// META: script=/wasm/component/jsapi/harness/assertions.js

// The exports object built by `create the exports object`, using a fixture
// with one export of every sort: a function, a resource type (with
// constructor and method), an instance, a value and a core module.

const FIXTURE = "instance/exports-all-sorts";
const KEYS = ["answer", "Counter", "nested", "theAnswer", "empty"];

let instance;
promise_setup(async () => {
  instance = await instantiateComponent(FIXTURE, { v: 5 });
});

promise_test(async () => {
  assert_ComponentInstance(instance, KEYS);
}, "exports: frozen null-prototype object with one key per export in declaration order");

promise_test(async () => {
  assert_equals(instance.exports, instance.exports);
  const getter = Object.getOwnPropertyDescriptor(WebAssembly.ComponentInstance.prototype, "exports").get;
  assert_equals(getter.call(instance), instance.exports);
}, "exports: the getter returns the same object every time");

promise_test(async () => {
  const { exports } = instance;
  assert_throws_js(TypeError, () => { "use strict"; exports.answer = 1; });
  assert_throws_js(TypeError, () => { "use strict"; exports.extra = 1; });
  assert_throws_js(TypeError, () => { "use strict"; delete exports.answer; });
  assert_throws_js(TypeError, () => Object.defineProperty(exports, "answer", { value: 1 }));
  assert_throws_js(TypeError, () => Object.setPrototypeOf(exports, {}));
  assert_array_equals(Reflect.ownKeys(exports), KEYS);
}, "exports: cannot be modified");

promise_test(async () => {
  const { answer } = instance.exports;
  assert_builtin_function(answer, { name: "answer", length: 0 }, "answer");
  assert_equals(answer(), 42);
  assert_equals(answer.call(null), 42);
}, "exports: a function export is a built-in function");

promise_test(async () => {
  const { Counter } = instance.exports;
  assert_guest_class(Counter, { name: "Counter", length: 1, methods: [["get", 0]] });
  const counter = new Counter(9);
  assert_equals(Object.getPrototypeOf(counter), Counter.prototype);
  assert_equals(counter.get(), 9);
}, "exports: a resource type export is a guest resource class");

promise_test(async () => {
  assert_false("[constructor]counter" in instance.exports);
  assert_false("[method]counter.get" in instance.exports);
  assert_false("counter" in instance.exports);
  assert_false("get" in instance.exports);
}, "exports: constructor and method exports are not properties of the exports object");

promise_test(async () => {
  const { nested } = instance.exports;
  assert_exports_object(nested, ["answer"], "nested");
  assert_builtin_function(nested.answer, { name: "answer", length: 0 }, "nested.answer");
  assert_equals(nested.answer(), 42);
}, "exports: an instance export is a nested exports object");

promise_test(async () => {
  assert_not_equals(instance.exports.nested.answer, instance.exports.answer);
}, "exports: the same function exported twice gets two JS functions");

promise_test(async () => {
  assert_equals(instance.exports.theAnswer, 5);
  const desc = Object.getOwnPropertyDescriptor(instance.exports, "theAnswer");
  assert_equals(desc.value, 5);
}, "exports: a value export is a data property holding the converted value");

promise_test(async () => {
  const { exports } = await instantiateComponent(FIXTURE, { v: "6" });
  assert_equals(exports.theAnswer, 6);
}, "exports: a value import is converted with ToComponentValue");

promise_test(async () => {
  const { empty } = instance.exports;
  assert_equals(Object.getPrototypeOf(empty), WebAssembly.Module.prototype);
  assert_array_equals(WebAssembly.Module.exports(empty).map(e => `${e.name}:${e.kind}`), ["nop:function"]);
  assert_array_equals(WebAssembly.Module.imports(empty), []);
  new WebAssembly.Instance(empty).exports.nop();
}, "exports: a core module export is a WebAssembly.Module");

promise_test(async () => {
  const other = await instantiateComponent(FIXTURE, { v: 5 });
  assert_not_equals(other.exports, instance.exports);
  assert_not_equals(other.exports.answer, instance.exports.answer);
  assert_not_equals(other.exports.Counter, instance.exports.Counter);
  assert_not_equals(other.exports.nested, instance.exports.nested);
  assert_not_equals(other.exports.empty, instance.exports.empty);
}, "exports: each instantiation gets fresh export objects");

promise_test(async () => {
  const { exports } = await instantiateComponent("instance/type-import", { Counter: class {} });
  assert_exports_object(exports, ["answer"]);
}, "exports: imported types do not appear in the exports object");

promise_test(async () => {
  const component = new WebAssembly.Component(emptyComponentBytes);
  const { exports } = new WebAssembly.ComponentInstance(component);
  assert_exports_object(exports, []);
}, "exports: a component with no exports still has a frozen null-prototype exports object");
