// META: global=window,dedicatedworker,jsshell
// META: script=/wasm/component/jsapi/harness/component-helpers.js
// META: script=/wasm/component/jsapi/harness/assertions.js

let exports;
let received;
promise_setup(async () => {
  exports = await componentExports("functions/arity", {
    recv(...args) { received = args; },
  });
});

promise_test(async () => {
  assert_builtin_function(exports.none, { name: "none", length: 0 }, "none");
  assert_builtin_function(exports.one, { name: "one", length: 1 }, "one");
  assert_builtin_function(exports.three, { name: "three", length: 3 }, "three");
}, "exported functions have length equal to their parameter count");

promise_test(async () => {
  assert_function_length(exports.Counter, 1, "Counter");
  assert_method_descriptor(exports.Counter.prototype, "add", { length: 2 }, "Counter.prototype.add");
  assert_method_descriptor(exports.Counter, "twice", { length: 1 }, "Counter.twice");
}, "a method's length excludes self; a constructor's and static's do not");

promise_test(async () => {
  assert_equals(exports.none(), undefined);
  assert_equals(exports.none(1, 2, 3), undefined);
}, "a function without a result returns undefined");

promise_test(async () => {
  assert_equals(exports.one(), 0);
  exports.three();
  assert_array_equals(received, [0, 0, 0]);
  exports.three(1);
  assert_array_equals(received, [1, 0, 0]);
  exports.three(1, 2);
  assert_array_equals(received, [1, 2, 0]);
}, "missing arguments are undefined, and are converted");

promise_test(async () => {
  assert_equals(exports.one(5, "ignored", Symbol()), 5);
  exports.three(1, 2, 3, Symbol(), {}, null);
  assert_array_equals(received, [1, 2, 3]);
}, "extra arguments are ignored and never converted");

promise_test(async () => {
  assert_equals(exports.one(undefined), 0);
  exports.three(1, undefined, 3);
  assert_array_equals(received, [1, 0, 3]);
}, "an explicit undefined is converted like a missing argument");

promise_test(async () => {
  exports.three(7, 8, 9);
  assert_equals(received.length, 3, "import receives exactly the parameter count");
  assert_array_equals(received, [7, 8, 9]);
}, "an import receives exactly one JS argument per parameter");

promise_test(async () => {
  const counter = new exports.Counter(10);
  assert_equals(counter.add(1, 2), 13);
  assert_equals(counter.add(1), 11, "a missing method argument is undefined");
  assert_equals(counter.add(1, 2, 3), 13, "extra method arguments are ignored");
  assert_equals(new exports.Counter().add(1, 2), 3, "a missing constructor argument is undefined");
  assert_equals(exports.Counter.twice(4), 8);
  assert_equals(exports.Counter.twice(), 0, "a missing static argument is undefined");
}, "missing arguments are undefined for constructors, methods and statics");

promise_test(async () => {
  const log = [];
  const arg = n => ({ valueOf() { log.push(n); return n; } });
  exports.three(arg(1), arg(2), arg(3));
  assert_array_equals(log, [1, 2, 3]);
}, "arguments are converted left to right");

promise_test(async () => {
  const log = [];
  received = undefined;
  assert_throws_js(TypeError, () => exports.three(
    { valueOf() { log.push(1); return 1; } },
    { valueOf() { log.push(2); return 1n; } },
    { valueOf() { log.push(3); return 3; } }));
  assert_array_equals(log, [1, 2], "conversion stops at the first failure");
  assert_equals(received, undefined, "recv must not be called");
}, "a failing conversion stops before later arguments and the call");

promise_test(async () => {
  const { one } = exports;
  assert_equals(one.call(null, 3), 3);
  assert_equals(one.call({}, 4), 4);
  assert_equals(Reflect.apply(one, undefined, [5]), 5);
}, "the this value of a plain export is ignored");
