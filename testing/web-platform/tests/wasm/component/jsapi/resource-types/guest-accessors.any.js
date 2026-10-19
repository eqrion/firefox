// META: global=window,dedicatedworker,jsshell
// META: script=/wasm/component/jsapi/harness/component-helpers.js
// META: script=/wasm/component/jsapi/harness/assertions.js

// `[method][get]`/`[method][set]` and `[static][get]`/`[static][set]`
// exports become accessor properties on the prototype and the class.

let exports;
promise_setup(async () => {
  exports = await componentExports("resources/guest-accessors");
});

function strictSet(object, key, value) {
  "use strict";
  object[key] = value;
}

promise_test(async () => {
  assert_guest_class(exports.Counter, {
    name: "Counter",
    length: 1,
    accessors: [["value", true], ["doubled", false]],
    staticAccessors: [["instances", true], ["version", false]],
  });
}, "class shape with accessors");

promise_test(async () => {
  const counter = new exports.Counter(5);
  assert_equals(counter.value, 5);
  counter.value = 9;
  assert_equals(counter.value, 9);
  assert_equals(counter.doubled, 18);
}, "prototype accessors read and write guest state");

promise_test(async () => {
  const counter = new exports.Counter(5);
  counter.value = "12";
  assert_equals(counter.value, 12);
  assert_throws_js(TypeError, () => { counter.value = -1; });
  assert_equals(counter.value, 12, "a failed set leaves the value unchanged");
}, "setter arguments are converted with ToComponentValue");

promise_test(async () => {
  const counter = new exports.Counter(5);
  assert_throws_js(TypeError, () => strictSet(counter, "doubled", 1));
  assert_equals(counter.doubled, 10);
}, "assigning to a getter-only accessor throws in strict mode");

promise_test(async () => {
  const { Counter } = exports;
  Counter.instances = 0;
  new Counter(1);
  new Counter(2);
  assert_equals(Counter.instances, 2);
  Counter.instances = 10;
  assert_equals(Counter.instances, 10);
  assert_equals(Counter.version, 7);
  assert_throws_js(TypeError, () => strictSet(Counter, "version", 1));
}, "static accessors read and write guest state");

promise_test(async () => {
  const { Counter } = exports;
  const counter = new Counter(3);
  const { get, set } = Object.getOwnPropertyDescriptor(Counter.prototype, "value");
  assert_equals(get.call(counter), 3);
  set.call(counter, 4);
  assert_equals(counter.value, 4);
  for (const thisValue of [undefined, {}, Counter.prototype, Counter]) {
    assert_throws_js(TypeError, () => get.call(thisValue), `getter this = ${format_value(thisValue)}`);
    assert_throws_js(TypeError, () => set.call(thisValue, 1), `setter this = ${format_value(thisValue)}`);
  }
}, "prototype accessor functions brand-check this");

promise_test(async () => {
  const { Counter } = exports;
  const { get, set } = Object.getOwnPropertyDescriptor(Counter, "instances");
  assert_equals(get.call(null), Counter.instances);
  set.call({}, 42);
  assert_equals(Counter.instances, 42);
  assert_equals(Object.getOwnPropertyDescriptor(Counter, "version").get.call(undefined), 7);
}, "static accessor functions ignore this");

promise_test(async () => {
  const counter = new exports.Counter(1);
  counter[Symbol.dispose]();
  assert_throws_js(TypeError, () => counter.value);
  assert_throws_js(TypeError, () => { counter.value = 2; });
}, "accessors on a dropped instance throw TypeError");

promise_test(async () => {
  const { Counter } = exports;
  class Sub extends Counter {}
  const sub = new Sub(4);
  assert_equals(sub.value, 4);
  sub.value = 6;
  assert_equals(sub.doubled, 12);
}, "accessors work on subclass instances");
