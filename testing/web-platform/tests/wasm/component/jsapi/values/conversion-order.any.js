// META: global=window,dedicatedworker,jsshell
// META: script=/wasm/component/jsapi/harness/component-helpers.js
// META: script=/wasm/component/jsapi/harness/assertions.js

// roundtrip(a: u32, b: u32, c: u32) -> u32 calls echo(a, b, c).

let roundtrip;
let calls;
promise_setup(async () => {
  const echo = (...args) => { calls.push(args); return args[0]; };
  ({ roundtrip } = await componentExports("values/conversion-order", { echo }));
});

function tracked(log, label, value) {
  return { valueOf() { log.push(label); return value; } };
}

promise_test(async () => {
  const log = [];
  calls = [];
  const result = roundtrip(tracked(log, "a", 1), tracked(log, "b", 2), tracked(log, "c", 3));
  assert_array_equals(log, ["a", "b", "c"]);
  assert_equals(calls.length, 1);
  assert_array_equals(calls[0], [1, 2, 3]);
  assert_equals(result, 1);
}, "arguments are converted left to right, then the import is called once");

promise_test(async () => {
  const log = [];
  calls = [];
  assert_throws_js(TypeError, () => roundtrip(tracked(log, "a", 1), Symbol(), tracked(log, "c", 3)));
  assert_array_equals(log, ["a"], "only the first argument was converted");
  assert_equals(calls.length, 0, "the import was not called");
}, "a failing second argument stops conversion before the third");

promise_test(async () => {
  const log = [];
  calls = [];
  assert_throws_js(TypeError, () => roundtrip(tracked(log, "a", 1), tracked(log, "b", 2), 1n));
  assert_array_equals(log, ["a", "b"]);
  assert_equals(calls.length, 0);
}, "a failing third argument still converts the first two");

promise_test(async () => {
  const log = [];
  calls = [];
  roundtrip(tracked(log, "a", 1), tracked(log, "b", 2));
  assert_array_equals(log, ["a", "b"]);
  assert_equals(calls.length, 1);
  assert_array_equals(calls[0], [1, 2, 0], "a missing argument converts from undefined");
}, "a missing argument is converted in order like undefined");

promise_test(async () => {
  const log = [];
  calls = [];
  roundtrip(1, 2, 3, tracked(log, "d", 4));
  assert_array_equals(log, [], "extra arguments are never converted");
  assert_array_equals(calls[0], [1, 2, 3]);
}, "extra arguments are ignored and not converted");

promise_test(async () => {
  const log = [];
  calls = [];
  class MyError extends Error { name = "MyError"; }
  assert_throws_js(MyError, () => roundtrip(tracked(log, "a", 1), { valueOf() { throw new MyError(); } }, tracked(log, "c", 3)));
  assert_array_equals(log, ["a"]);
  assert_equals(calls.length, 0);
}, "an exception from a conversion propagates unchanged");
