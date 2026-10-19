// META: global=window,dedicatedworker,jsshell
// META: script=/wasm/component/jsapi/harness/component-helpers.js
// META: script=/wasm/component/jsapi/harness/assertions.js

// The JS class created for an exported resource type, per
// `create a guest resource class`.

let exports;
let noCtorExports;
promise_setup(async () => {
  exports = await componentExports("resources/guest-counter", { notify() {} });
  noCtorExports = await componentExports("resources/guest-no-ctor");
});

promise_test(async () => {
  assert_exports_object(exports, [
    "Counter", "dropCount", "make", "take", "peek", "peekPair", "takeBoth", "takeThenBorrow",
    "makePair", "sameRep", "borrowAndNotify", "takeList",
  ]);
}, "member exports are attached to the class instead of the exports object");

promise_test(async () => {
  assert_guest_class(exports.Counter, {
    name: "Counter",
    length: 1,
    methods: [["increment", 0], ["value", 0], ["clone", 0], ["notify", 0]],
    statics: [["create", 1]],
  });
}, "class shape with a constructor, methods and a static");

promise_test(async () => {
  assert_guest_class(noCtorExports.Counter, {
    name: "Counter",
    length: 0,
    methods: [["value", 0]],
  });
}, "class shape without a [constructor] export has length 0");

promise_test(async () => {
  const { Counter } = exports;
  const counter = new Counter(0);
  assert_equals(Object.getPrototypeOf(counter), Counter.prototype);
  assert_true(counter instanceof Counter);
  assert_true(Object.isExtensible(counter));
  assert_array_equals(Reflect.ownKeys(counter), [], "no own properties");
}, "instances have the class prototype and no own properties");

promise_test(async () => {
  const counter = new exports.Counter(0);
  assert_equals(Object.prototype.toString.call(counter), "[object Counter]");
  assert_equals(Object.prototype.toString.call(exports.Counter.prototype), "[object Counter]");
}, "Symbol.toStringTag gives [object Counter]");

promise_test(async () => {
  const dispose = exports.Counter.prototype[Symbol.dispose];
  assert_equals(typeof dispose, "function");
  assert_equals(Object.getPrototypeOf(dispose), Function.prototype);
  assert_function_name(dispose, "[Symbol.dispose]", "Symbol.dispose");
  assert_function_length(dispose, 0, "Symbol.dispose");
  assert_throws_js(TypeError, () => new dispose());
}, "Symbol.dispose is a built-in function named [Symbol.dispose] of length 0");

promise_test(async () => {
  let usingBlock;
  try {
    usingBlock = new Function("Counter", "{ using c = new Counter(1); return c; }");
  } catch (e) {
    assert_implements_optional(false, "using declarations are not supported");
  }
  const before = exports.dropCount();
  const counter = usingBlock(exports.Counter);
  assert_equals(exports.dropCount(), before + 1, "leaving the block disposes");
  assert_throws_js(TypeError, () => counter.value(), "disposed instance is unusable");
}, "instances work with using declarations");

promise_test(async () => {
  const { Counter } = exports;
  assert_equals(Counter.prototype.increment, Counter.prototype.increment, "methods are stable");
  assert_not_equals(Counter.prototype.increment, Counter.prototype.value, "methods are distinct");
  assert_equals(Counter.create, Counter.create, "statics are stable");
}, "methods and statics are stable function objects");
