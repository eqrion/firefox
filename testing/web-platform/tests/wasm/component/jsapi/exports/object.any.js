// META: global=window,dedicatedworker,jsshell
// META: script=/wasm/component/jsapi/harness/component-helpers.js
// META: script=/wasm/component/jsapi/harness/assertions.js

const KEYS = ["zeta", "Counter", "alphaBeta", "v", "innerModule", "test:pkg/iface@1.2.3", "omega"];

promise_test(async () => {
  const instance = await instantiateComponent("exports/object", { v: "hello" });
  assert_ComponentInstance(instance, KEYS);
}, "the exports object is frozen, null-prototype and in declaration order");

for_each_entry_point(async (t, instantiate) => {
  const instance = await instantiate("exports/object", { v: "hello" });
  assert_array_equals(Reflect.ownKeys(instance.exports), KEYS);
}, "export keys are the same through every entry point");

promise_test(async () => {
  const exports = await componentExports("exports/object", { v: "hello" });
  assert_builtin_function(exports.zeta, { name: "zeta", length: 0 }, "zeta");
  assert_equals(exports.zeta(), 42);
  assert_equals(typeof exports.Counter, "function", "Counter is a class");
  assert_function_name(exports.Counter, "Counter", "Counter");
  assert_exports_object(exports.alphaBeta, ["f"], "alphaBeta");
  assert_equals(exports.alphaBeta.f(), 42);
  assert_equals(exports.v, "hello");
  assert_equals(Object.getPrototypeOf(exports.innerModule), WebAssembly.Module.prototype);
  assert_exports_object(exports["test:pkg/iface@1.2.3"], ["f"], "test:pkg/iface@1.2.3");
  assert_builtin_function(exports.omega, { name: "omega", length: 0 }, "omega");
}, "every export sort gets its JS definition");

promise_test(async () => {
  const exports = await componentExports("exports/object", { v: "hello" });
  assert_throws_js(TypeError, () => { "use strict"; exports.zeta = 1; }, "assignment");
  assert_throws_js(TypeError, () => { "use strict"; exports.extra = 1; }, "adding a property");
  assert_throws_js(TypeError, () => { "use strict"; delete exports.zeta; }, "deletion");
  assert_throws_js(TypeError, () => Object.defineProperty(exports, "zeta", { value: 1 }), "defineProperty");
  assert_throws_js(TypeError, () => Object.setPrototypeOf(exports, {}), "setPrototypeOf");
  assert_array_equals(Reflect.ownKeys(exports), KEYS, "keys unchanged");
}, "the exports object rejects every mutation");

promise_test(async () => {
  const a = await instantiateComponent("exports/object", { v: "a" });
  const b = await instantiateComponent("exports/object", { v: "b" });
  assert_not_equals(a.exports, b.exports, "exports objects");
  assert_not_equals(a.exports.zeta, b.exports.zeta, "functions");
  assert_not_equals(a.exports.Counter, b.exports.Counter, "classes");
  assert_not_equals(a.exports.alphaBeta, b.exports.alphaBeta, "instance objects");
  assert_not_equals(a.exports.innerModule, b.exports.innerModule, "core modules");
  assert_equals(a.exports.v, "a");
  assert_equals(b.exports.v, "b");
}, "each instantiation gets fresh export definitions");

promise_test(async () => {
  const instance = await instantiateComponent("exports/object-empty");
  assert_ComponentInstance(instance, []);
}, "a component with no exports has an empty frozen exports object");

promise_test(async () => {
  const instance = await instantiateComponent("exports/object-non-resource-type");
  assert_ComponentInstance(instance, []);
}, "a type export that is not a resource is left off the exports object");
