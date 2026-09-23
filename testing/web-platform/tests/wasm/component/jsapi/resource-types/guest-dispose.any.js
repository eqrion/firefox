// META: global=window,dedicatedworker,jsshell
// META: script=/wasm/component/jsapi/harness/component-helpers.js
// META: script=/wasm/component/jsapi/harness/assertions.js

// `drop a guest resource instance` via Symbol.dispose.

let exports;
promise_setup(async () => {
  exports = await componentExports("resources/guest-counter", { notify() {} });
});

promise_test(async () => {
  const { Counter, dropCount } = exports;
  const counter = new Counter(1);
  const before = dropCount();
  assert_equals(counter[Symbol.dispose](), undefined);
  assert_equals(dropCount(), before + 1, "the destructor ran");
  assert_equals(counter[Symbol.dispose](), undefined);
  assert_equals(dropCount(), before + 1, "a second dispose is a no-op");
}, "dispose runs the destructor exactly once");

promise_test(async () => {
  const { Counter, take, peek } = exports;
  const counter = new Counter(1);
  counter[Symbol.dispose]();
  assert_throws_js(TypeError, () => counter.value());
  assert_throws_js(TypeError, () => take(counter));
  assert_throws_js(TypeError, () => peek(counter));
}, "a disposed instance is rejected everywhere");

promise_test(async () => {
  const { Counter } = exports;
  const dispose = Counter.prototype[Symbol.dispose];
  for (const thisValue of [undefined, null, {}, Counter.prototype, Object.create(Counter.prototype),
                           Counter, 1]) {
    assert_throws_js(TypeError, () => dispose.call(thisValue), `this = ${format_value(thisValue)}`);
  }
}, "dispose brand-checks this");

promise_test(async () => {
  const { Counter, dropCount } = exports;
  const counter = new Counter(1);
  const before = dropCount();
  Counter.prototype[Symbol.dispose].call(counter);
  assert_equals(dropCount(), before + 1);
}, "dispose works when extracted from the prototype");

promise_test(async () => {
  const { Counter, dropCount } = exports;
  class Sub extends Counter {}
  const sub = new Sub(1);
  const before = dropCount();
  sub[Symbol.dispose]();
  assert_equals(dropCount(), before + 1);
}, "dispose works on subclass instances");

promise_test(async () => {
  const { Counter } = await componentExports("resources/guest-trapping-dtor");
  const counter = new Counter();
  assert_traps(() => counter[Symbol.dispose]());
  assert_traps(() => new Counter(), "the instance is locked down after the trap");
}, "a trapping destructor throws RuntimeError from dispose");

promise_test(async () => {
  const { Counter, take } = await componentExports("resources/guest-trapping-dtor");
  const counter = new Counter();
  assert_traps(() => take(counter));
  assert_traps(() => new Counter(), "the instance is locked down after the trap");
}, "a trapping destructor throws RuntimeError when the guest drops the handle");

promise_test(async () => {
  const { make, dropCount } = exports;
  const counter = make();
  const before = dropCount();
  counter[Symbol.dispose]();
  assert_equals(dropCount(), before + 1);
}, "instances lifted from an export can be disposed");
