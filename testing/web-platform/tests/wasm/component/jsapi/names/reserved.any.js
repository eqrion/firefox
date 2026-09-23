// META: global=window,dedicatedworker,jsshell
// META: script=/wasm/component/jsapi/harness/component-helpers.js
// META: script=/wasm/component/jsapi/harness/assertions.js

test_instantiation_error(TypeError, "names/reserved-method-constructor", undefined, undefined,
                         "a [method] named `constructor` throws a TypeError");

test_instantiation_error(TypeError, "names/reserved-static-prototype", undefined, undefined,
                         "a [static] named `prototype` throws a TypeError");

promise_test(async () => {
  const { Counter } = await componentExports("names/reserved-allowed");
  assert_guest_class(Counter, { name: "Counter", methods: [["prototype", 0]], statics: [["constructor", 0]] });
  assert_equals(Counter.prototype.constructor, Counter, "prototype.constructor is untouched");
  assert_equals(Counter.constructor(), 3, "a static named constructor is an ordinary static");
  assert_equals(new Counter().prototype(), 2, "a method named prototype is an ordinary method");
}, "`constructor` as a static and `prototype` as a method are not reserved");

promise_test(async () => {
  let receiver;
  function R() {}
  R.prototype = function proto() { return 7; };
  R.prototype.constructor = function ctor() { receiver = this; return 5; };
  const { callConstructor, callPrototype } = await componentExports("names/reserved-imports", { R });
  const r = new R();
  assert_equals(callConstructor(r), 5, "[method]r.constructor reads R.prototype.constructor");
  assert_equals(receiver, r, "and calls it with self as this");
  assert_equals(callPrototype(), 7, "[static]r.prototype reads R.prototype");
}, "`constructor` and `prototype` are not reserved for imports");
