// META: global=window,dedicatedworker,jsshell
// META: script=/wasm/component/jsapi/harness/component-helpers.js
// META: script=/wasm/component/jsapi/harness/assertions.js

// Requires wasm-tools with [get]/[set] support (wasm-tools#2660).

let exports;
promise_setup(async () => {
  exports = await componentExports("exports/accessors");
});

promise_test(async () => {
  assert_array_equals(Reflect.ownKeys(exports), ["counter", "readOnly", "plain"]);
}, "property exports take one key per label, in declaration order");

promise_test(async () => {
  assert_accessor_descriptor(exports, "counter", { settable: true, enumerable: true, configurable: false },
                             "exports.counter");
  assert_accessor_descriptor(exports, "readOnly", { settable: false, enumerable: true, configurable: false },
                             "exports.readOnly");
}, "[get]/[set] exports become accessor properties on the exports object");

promise_test(async () => {
  assert_equals(exports.counter, 0);
  exports.counter = 5;
  assert_equals(exports.counter, 5);
  exports.counter = "9";
  assert_equals(exports.counter, 9, "setter converts like a parameter");
  assert_equals(exports.readOnly, 7);
}, "getters and setters call the component functions");

promise_test(async () => {
  assert_throws_js(TypeError, () => { exports.counter = -1; });
  assert_throws_js(TypeError, () => { exports.counter = undefined; });
}, "a setter argument that fails conversion throws a TypeError");

promise_test(async () => {
  assert_throws_js(TypeError, () => { "use strict"; exports.readOnly = 1; });
  assert_equals(exports.readOnly, 7);
}, "assigning to a getter-only property throws in strict mode");

promise_test(async () => {
  const { get, set } = Object.getOwnPropertyDescriptor(exports, "counter");
  set.call(undefined, 3);
  assert_equals(get.call(undefined), 3, "undefined this");
  set.call({}, 4);
  assert_equals(get.call({}), 4, "unrelated this");
  assert_equals(get(), 4, "no this");
}, "accessor functions ignore their this value");

promise_test(async () => {
  assert_builtin_function(exports.plain, { name: "plain", length: 0 }, "plain");
  const desc = Object.getOwnPropertyDescriptor(exports, "plain");
  assert_true(Object.hasOwn(desc, "value"), "plain is a data property");
}, "a plain export next to property exports is still a data property");
