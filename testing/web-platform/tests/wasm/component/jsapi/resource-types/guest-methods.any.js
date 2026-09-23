// META: global=window,dedicatedworker,jsshell
// META: script=/wasm/component/jsapi/harness/component-helpers.js
// META: script=/wasm/component/jsapi/harness/assertions.js

// Methods and statics installed by `create a guest resource class`.

let exports;
let otherExports;
promise_setup(async () => {
  exports = await componentExports("resources/guest-counter", { notify() {} });
  otherExports = await componentExports("resources/guest-no-ctor");
});

promise_test(async () => {
  const counter = new exports.Counter(0);
  assert_equals(counter.increment(), 1);
  assert_equals(counter.increment(), 2);
  assert_equals(counter.value(), 2);
}, "methods receive the instance as self");

promise_test(async () => {
  const { Counter } = exports;
  const { increment } = Counter.prototype;
  for (const thisValue of [undefined, null, {}, Counter.prototype, Object.create(Counter.prototype),
                           Counter, 1, "x"]) {
    assert_throws_js(TypeError, () => increment.call(thisValue), `this = ${format_value(thisValue)}`);
  }
}, "methods brand-check this");

promise_test(async () => {
  const other = otherExports.make();
  assert_throws_js(TypeError, () => exports.Counter.prototype.value.call(other));
  assert_throws_js(TypeError, () => otherExports.Counter.prototype.value.call(new exports.Counter(0)));
}, "methods reject instances of another component's class");

promise_test(async () => {
  const { Counter } = exports;
  const counter = new Counter(10);
  const { value } = Counter.prototype;
  assert_equals(value.call(counter), 10);
  assert_equals(Reflect.apply(value, counter, []), 10);
  assert_throws_js(TypeError, () => value());
}, "extracted methods work with an explicit this");

promise_test(async () => {
  const counter = new exports.Counter(0);
  exports.take(counter);
  assert_throws_js(TypeError, () => counter.value());
  assert_throws_js(TypeError, () => counter.increment());
}, "methods on a dropped instance throw TypeError");

promise_test(async () => {
  const { Counter } = exports;
  assert_equals(Counter.create(3).value(), 3);
  assert_true(Counter.create(3) instanceof Counter);
  const { create } = Counter;
  assert_equals(create(4).value(), 4, "statics do not need a this");
  assert_equals(Counter.create.call({}, 5).value(), 5, "statics ignore this");
  assert_equals(Counter.create().value(), 0, "a missing static argument is undefined");
}, "statics are plain functions on the class");

promise_test(async () => {
  const { Counter } = exports;
  const counter = new Counter(7);
  const clone = counter.clone();
  assert_not_equals(clone, counter);
  assert_true(clone instanceof Counter);
  assert_equals(clone.value(), 7);
  clone.increment();
  assert_equals(counter.value(), 7, "the clone is an independent resource");
}, "a method returning own gives a fresh instance");

promise_test(async () => {
  const counter = new exports.Counter(0);
  assert_equals(counter.increment(1, 2, 3), 1);
}, "extra method arguments are ignored");

promise_test(async () => {
  const counter = exports.make();
  assert_true(counter instanceof exports.Counter);
  assert_equals(counter.value(), 0);
}, "instances lifted from a plain export are usable with methods");

promise_test(async () => {
  const { Counter } = exports;
  const counter = new Counter(0);
  class Sub extends Counter {}
  const sub = new Sub(9);
  assert_equals(Counter.prototype.value.call(sub), 9);
  assert_equals(Counter.prototype.value.call(Object.setPrototypeOf(counter, null)), 0,
                "brand check does not use the prototype chain");
}, "brand check is by internal slot, not prototype");
