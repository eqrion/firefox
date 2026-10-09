// META: global=window,dedicatedworker,jsshell
// META: script=/wasm/component/jsapi/harness/component-helpers.js
// META: script=/wasm/component/jsapi/harness/assertions.js

// [[Construct]] behaviour of a guest resource class.

let exports;
promise_setup(async () => {
  exports = await componentExports("resources/guest-counter", { notify() {} });
});

promise_test(async () => {
  const counter = new exports.Counter(5);
  assert_equals(counter.value(), 5);
  assert_equals(counter.increment(), 6);
}, "new invokes the [constructor] export with converted arguments");

promise_test(async () => {
  assert_equals(new exports.Counter().value(), 0);
}, "missing constructor arguments are undefined");

promise_test(async () => {
  assert_equals(new exports.Counter(1, 2, 3).value(), 1);
}, "extra constructor arguments are ignored");

promise_test(async () => {
  assert_equals(new exports.Counter("7").value(), 7);
  assert_equals(new exports.Counter(2.9).value(), 2);
}, "constructor arguments are converted with ToComponentValue");

promise_test(async () => {
  assert_throws_js(TypeError, () => new exports.Counter(1n));
  assert_throws_js(TypeError, () => new exports.Counter(Symbol()));
}, "constructor arguments that fail conversion throw TypeError");

promise_test(async () => {
  const { Counter } = await componentExports("resources/guest-no-ctor");
  assert_throws_js(TypeError, () => new Counter());
  assert_throws_js(TypeError, () => Counter());
}, "a class without a [constructor] export throws TypeError on new");

promise_test(async () => {
  const { Counter } = exports;
  function NewTarget() {}
  NewTarget.prototype = { custom: true };
  const counter = Reflect.construct(Counter, [3], NewTarget);
  assert_equals(Object.getPrototypeOf(counter), NewTarget.prototype);
  assert_true(counter.custom);
  assert_false(counter instanceof Counter);
  assert_equals(Counter.prototype.value.call(counter), 3, "still a real instance");
}, "newTarget.prototype is used as the prototype");

promise_test(async () => {
  const { Counter } = exports;
  for (const prototype of [null, undefined, 1, "x"]) {
    function NewTarget() {}
    NewTarget.prototype = prototype;
    const counter = Reflect.construct(Counter, [3], NewTarget);
    assert_equals(Object.getPrototypeOf(counter), Counter.prototype, `prototype ${format_value(prototype)}`);
  }
}, "a non-object newTarget.prototype falls back to the class prototype");

promise_test(async () => {
  const { Counter } = exports;
  class Sub extends Counter {
    constructor() {
      super(3);
    }
    plusOne() {
      return this.value() + 1;
    }
  }
  const sub = new Sub();
  assert_true(sub instanceof Sub);
  assert_true(sub instanceof Counter);
  assert_equals(Object.getPrototypeOf(sub), Sub.prototype);
  assert_equals(sub.plusOne(), 4);
  assert_equals(sub.increment(), 4);
  assert_equals(exports.peek(sub), 4, "subclass instances are accepted as borrows");
}, "class extends works");

promise_test(async () => {
  const { Counter, dropCount } = await componentExports("resources/guest-result-ctor");
  assert_equals(new Counter(4).value(), 4);
  const before = dropCount();
  assert_component_error(() => new Counter(0), "zero");
  assert_equals(dropCount(), before, "no resource is created for the error case");
}, "a [constructor] returning result throws ComponentError for the error case");

promise_test(async () => {
  const { Counter, dropCount } = await componentExports("resources/guest-result-ctor");
  assert_traps(() => new Counter(0xffffffff));
  assert_traps(() => dropCount(), "instance is locked down after a trap");
  assert_traps(() => new Counter(1), "constructing is locked down after a trap");
}, "a trapping [constructor] throws RuntimeError and locks the instance down");

promise_test(async () => {
  const { Counter } = exports;
  assert_throws_js(TypeError, () => Counter(1));
  assert_throws_js(TypeError, () => Counter.call({}, 1));
  assert_throws_js(TypeError, () => Reflect.apply(Counter, undefined, [1]));
}, "[[Call]] throws TypeError");

promise_test(async () => {
  const { Counter } = exports;
  const a = new Counter(1);
  const b = new Counter(1);
  assert_not_equals(a, b);
  a.increment();
  assert_equals(b.value(), 1, "instances have independent state");
}, "each construction creates an independent resource");
