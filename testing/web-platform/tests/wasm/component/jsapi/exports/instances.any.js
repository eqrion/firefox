// META: global=window,dedicatedworker,jsshell
// META: script=/wasm/component/jsapi/harness/component-helpers.js
// META: script=/wasm/component/jsapi/harness/assertions.js

const HANDLER = "test:pkg/handler@0.2.0";

let exports;
promise_setup(async () => {
  exports = await componentExports("exports/instances");
});

promise_test(async () => {
  assert_array_equals(Reflect.ownKeys(exports), ["Counter", "iface", HANDLER]);
}, "instance exports are keyed by their JS name");

promise_test(async () => {
  assert_exports_object(exports.iface, ["f", "inner", "Counter"], "iface");
  assert_exports_object(exports.iface.inner, ["g"], "iface.inner");
  assert_exports_object(exports[HANDLER], ["g"], HANDLER);
}, "exported instances are frozen null-prototype objects in declaration order");

promise_test(async () => {
  assert_builtin_function(exports.iface.f, { name: "f", length: 0 }, "iface.f");
  assert_equals(exports.iface.f(), 1);
  assert_equals(exports.iface.inner.g(), 2);
  assert_equals(exports[HANDLER].g(), 2);
}, "functions inside exported instances work");

promise_test(async () => {
  assert_equals(exports.iface.Counter, exports.Counter);
}, "a resource type exported both at top level and inside an instance is one class");

promise_test(async () => {
  assert_not_equals(exports[HANDLER], exports.iface.inner);
  assert_not_equals(exports[HANDLER].g, exports.iface.inner.g);
}, "the same instance exported twice gets two exports objects");

promise_test(async () => {
  assert_throws_js(TypeError, () => { "use strict"; exports.iface.f = 1; });
  assert_throws_js(TypeError, () => { "use strict"; exports.iface.inner.extra = 1; });
}, "nested exports objects reject mutation");
