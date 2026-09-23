// META: global=window,dedicatedworker,jsshell
// META: script=/wasm/component/jsapi/harness/component-helpers.js
// META: script=/wasm/component/jsapi/harness/assertions.js

// Passing guest resource instances as `borrow` lends them for the duration
// of the call, tracked by the current lender list.

let exports;
let onNotify = () => {};
promise_setup(async () => {
  exports = await componentExports("resources/guest-counter", { notify: v => onNotify(v) });
});

promise_test(async () => {
  const { Counter, peek } = exports;
  const counter = new Counter(4);
  assert_equals(peek(counter), 4);
  assert_equals(peek(counter), 4, "still usable after the call");
  assert_equals(counter.value(), 4);
}, "borrowing leaves the instance usable");

promise_test(async () => {
  const { Counter, peekPair } = exports;
  const counter = new Counter(4);
  assert_equals(peekPair(counter, counter), 8);
  assert_equals(counter.value(), 4);
}, "the same instance can be lent twice in one call");

// The reentrant checks below run inside the `notify` import. They record
// outcomes instead of asserting there, because an exception thrown by an
// import with no result type is a trap that would lock the shared instance.
function outcome(fn) {
  try {
    return { value: fn() };
  } catch (e) {
    return { error: e.constructor };
  }
}

promise_test(async t => {
  const { Counter, borrowAndNotify, take, peek, dropCount } = exports;
  const counter = new Counter(6);
  const seen = [];
  onNotify = value => {
    seen.push(value);
    seen.push(outcome(() => counter[Symbol.dispose]()));
    seen.push(outcome(() => take(counter)));
    seen.push(outcome(() => peek(counter)));
  };
  t.add_cleanup(() => { onNotify = () => {}; });
  borrowAndNotify(counter);
  assert_equals(seen.length, 4, "notify ran once");
  assert_equals(seen[0], 6);
  assert_equals(seen[1].error, TypeError, "dispose while lent");
  assert_equals(seen[2].error, TypeError, "own while lent");
  assert_equals(seen[3].value, 6, "borrowing again while lent");
  const before = dropCount();
  counter[Symbol.dispose]();
  assert_equals(dropCount(), before + 1, "dispose works after the call returns");
}, "a lent instance cannot be dropped or transferred while lent");

promise_test(async t => {
  const { Counter, take } = exports;
  const counter = new Counter(6);
  const seen = [];
  onNotify = () => {
    seen.push(outcome(() => counter[Symbol.dispose]()));
    seen.push(outcome(() => take(counter)));
  };
  t.add_cleanup(() => { onNotify = () => {}; });
  counter.notify();
  assert_equals(seen.length, 2, "notify ran once");
  assert_equals(seen[0].error, TypeError, "dispose of this while the method runs");
  assert_equals(seen[1].error, TypeError, "own of this while the method runs");
  counter[Symbol.dispose]();
}, "a method's this is lent for the duration of the call");

promise_test(async t => {
  const { Counter, borrowAndNotify, peek, take } = exports;
  const outer = new Counter(1);
  const inner = new Counter(2);
  const seen = [];
  onNotify = value => {
    if (value === 1) {
      borrowAndNotify(inner);
      seen.push(["outer dispose after nested call", outcome(() => outer[Symbol.dispose]())]);
      seen.push(["inner dispose after nested call", outcome(() => inner[Symbol.dispose]())]);
    } else {
      seen.push(["value in nested call", { value }]);
      seen.push(["inner dispose in nested call", outcome(() => inner[Symbol.dispose]())]);
      seen.push(["outer own in nested call", outcome(() => take(outer))]);
      seen.push(["outer borrow in nested call", outcome(() => peek(outer))]);
    }
  };
  t.add_cleanup(() => { onNotify = () => {}; });
  borrowAndNotify(outer);
  const expected = [
    ["value in nested call", { value: 2 }],
    ["inner dispose in nested call", { error: TypeError }],
    ["outer own in nested call", { error: TypeError }],
    ["outer borrow in nested call", { value: 1 }],
    ["outer dispose after nested call", { error: TypeError }],
    ["inner dispose after nested call", { value: undefined }],
  ];
  assert_equals(seen.length, expected.length, "both notify calls ran");
  for (let i = 0; i < expected.length; i++) {
    assert_equals(seen[i][0], expected[i][0], `step ${i}`);
    assert_equals(seen[i][1].error, expected[i][1].error, `${expected[i][0]}: error`);
    assert_equals(seen[i][1].value, expected[i][1].value, `${expected[i][0]}: value`);
  }
  outer[Symbol.dispose]();
}, "the lender list is restored after a nested call");

promise_test(async () => {
  const { Counter, takeBoth, dropCount } = exports;
  const counter = new Counter(1);
  assert_throws_js(TypeError, () => takeBoth(counter, {}));
  const before = dropCount();
  counter[Symbol.dispose]();
  assert_equals(dropCount(), before + 1, "the lend count was restored after the failed conversion");
}, "the lender list is restored when a later argument fails to convert");

promise_test(async () => {
  const { Counter, peek } = exports;
  for (const value of [undefined, null, {}, Counter.prototype, Object.create(Counter.prototype), 1]) {
    assert_throws_js(TypeError, () => peek(value), `borrow = ${format_value(value)}`);
  }
}, "values that are not instances are rejected as borrow");

promise_test(async () => {
  const { Counter, peek } = exports;
  const counter = new Counter(1);
  counter[Symbol.dispose]();
  assert_throws_js(TypeError, () => peek(counter));
}, "a dropped instance is rejected as borrow");

promise_test(async () => {
  const { make, peek } = exports;
  assert_equals(peek(make()), 0);
}, "an instance lifted from an export can be borrowed");
