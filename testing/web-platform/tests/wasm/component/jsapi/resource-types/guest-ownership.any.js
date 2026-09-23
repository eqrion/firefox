// META: global=window,dedicatedworker,jsshell
// META: script=/wasm/component/jsapi/harness/component-helpers.js
// META: script=/wasm/component/jsapi/harness/assertions.js

// Passing guest resource instances as `own` transfers the handle out of the
// JS instance, per `ToComponentValue(jsValue, own<R>)`.

let exports;
promise_setup(async () => {
  exports = await componentExports("resources/guest-counter", { notify() {} });
});

promise_test(async () => {
  const { Counter, take, dropCount } = exports;
  const counter = new Counter(1);
  const before = dropCount();
  assert_equals(take(counter), undefined);
  assert_equals(dropCount(), before + 1, "the guest dropped the resource");
  assert_throws_js(TypeError, () => counter.value(), "methods after transfer");
  assert_throws_js(TypeError, () => take(counter), "passing again");
  assert_throws_js(TypeError, () => exports.peek(counter), "borrowing after transfer");
  assert_equals(counter[Symbol.dispose](), undefined);
  assert_equals(dropCount(), before + 1, "dispose after transfer is a no-op");
}, "passing an instance as own transfers ownership");

promise_test(async () => {
  const { make } = exports;
  const a = make();
  const b = make();
  assert_not_equals(a, b);
  assert_equals(a.value(), 0);
  assert_equals(b.value(), 0);
}, "each lift of an own creates a fresh instance");

promise_test(async () => {
  const { sameRep, dropCount } = exports;
  const a = sameRep();
  const b = sameRep();
  assert_not_equals(a, b, "two lifts of the same rep are distinct objects");
  assert_equals(a.value(), 0);
  assert_equals(b.value(), 0);
  const before = dropCount();
  a[Symbol.dispose]();
  assert_equals(dropCount(), before + 1);
  assert_equals(b.value(), 0, "the other instance is unaffected");
}, "lifting the same rep twice gives two independent instances");

promise_test(async () => {
  const { makePair, Counter } = exports;
  const pair = makePair();
  assert_true(Array.isArray(pair));
  assert_equals(pair.length, 2);
  assert_not_equals(pair[0], pair[1]);
  assert_true(pair[0] instanceof Counter);
  assert_equals(pair[0].value(), 1);
  assert_equals(pair[1].value(), 2);
}, "owns inside a tuple result are lifted into instances");

promise_test(async () => {
  const { Counter, takeBoth, peek, take, dropCount } = exports;
  const counter = new Counter(3);
  const before = dropCount();
  assert_throws_js(TypeError, () => takeBoth(counter, counter));
  assert_equals(dropCount(), before, "nothing was dropped");
  assert_equals(peek(counter), 3, "the instance is intact after the failed call");
  take(counter);
  assert_equals(dropCount(), before + 1, "the lend count was restored");
}, "(borrow, own) of the same instance throws TypeError because it is lent");

// Lowering an own adds a handle to the guest's table, which enters the guest,
// so a failure on a later argument locks the instance down.
promise_test(async () => {
  const { Counter, takeThenBorrow, dropCount, peek } = await componentExports("resources/guest-counter", { notify() {} });
  const counter = new Counter(3);
  const before = dropCount();
  assert_throws_js(TypeError, () => takeThenBorrow(counter, counter));
  assert_traps(() => dropCount(), "the instance is locked down");
  assert_traps(() => peek(new Counter(1)), "every later call traps");
  // Disposing a consumed instance does nothing, even though the instance is
  // locked down.
  assert_equals(counter[Symbol.dispose](), undefined, "the consumed instance stays empty");
}, "(own, borrow) of the same instance throws TypeError and locks the instance down");

promise_test(async () => {
  const { Counter, takeList, dropCount } = exports;
  const a = new Counter(1);
  const b = new Counter(2);
  const before = dropCount();
  assert_equals(takeList([a, b]), 2);
  assert_equals(dropCount(), before + 2);
  assert_throws_js(TypeError, () => a.value());
  assert_throws_js(TypeError, () => b.value());
}, "owns inside a list parameter are all transferred");

promise_test(async () => {
  const { Counter, takeList, dropCount } = await componentExports("resources/guest-counter", { notify() {} });
  const a = new Counter(1);
  assert_throws_js(TypeError, () => takeList([a, {}]));
  assert_traps(() => dropCount(), "the instance is locked down");
  assert_equals(a[Symbol.dispose](), undefined, "the consumed instance stays empty");
}, "a list with a later invalid element throws TypeError and locks the instance down");

// A list is allocated in the guest before its elements are converted, so only
// a value that isn't a list fails before the guest is entered.
promise_test(async () => {
  const { Counter, takeList, dropCount, peek } = await componentExports("resources/guest-counter", { notify() {} });
  const a = new Counter(1);
  const before = dropCount();
  assert_throws_js(TypeError, () => takeList(a));
  assert_equals(dropCount(), before, "nothing was dropped");
  assert_equals(peek(a), 1, "the instance is still usable");
}, "a value that is not a list fails before the guest is entered");

promise_test(async () => {
  const { Counter, take } = exports;
  for (const value of [undefined, null, {}, Counter.prototype, Object.create(Counter.prototype),
                       Counter, 1, "counter"]) {
    assert_throws_js(TypeError, () => take(value), `own = ${format_value(value)}`);
  }
}, "values that are not instances are rejected as own");

promise_test(async () => {
  const { Counter, take, dropCount } = exports;
  class Sub extends Counter {}
  const sub = new Sub(1);
  const before = dropCount();
  take(sub);
  assert_equals(dropCount(), before + 1);
}, "subclass instances can be passed as own");

promise_test(async () => {
  const { make, take, dropCount } = exports;
  const counter = make();
  const before = dropCount();
  take(counter);
  assert_equals(dropCount(), before + 1);
}, "an instance lifted from an export can be transferred back");
