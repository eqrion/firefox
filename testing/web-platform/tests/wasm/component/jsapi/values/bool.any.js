// META: global=window,dedicatedworker,jsshell
// META: script=/wasm/component/jsapi/harness/component-helpers.js
// META: script=/wasm/component/jsapi/harness/assertions.js

let roundtrip;
let send;
let received;
const spied = [];
promise_setup(async () => {
  const echo = x => { received = x; return x; };
  const recv = x => { spied.push(x); };
  ({ roundtrip, send } = await componentExports("values/bool", { echo, recv }));
});

for (const value of [true, false]) {
  promise_test(async () => {
    const result = roundtrip(value);
    assert_equals(result, value);
    assert_equals(typeof result, "boolean");
    assert_equals(received, value, "value seen by echo");
  }, `bool: ${value} roundtrips exactly`);
}

for (const [input, description] of [
  [0, "0"],
  [-0, "-0"],
  [0n, "0n"],
  ["", "empty string"],
  [null, "null"],
  [undefined, "undefined"],
  [NaN, "NaN"],
]) {
  promise_test(async () => {
    assert_equals(roundtrip(input), false);
    assert_equals(received, false, "value seen by echo");
  }, `bool: ${description} is false under ToBoolean`);
}

for (const [input, description] of [
  [1, "1"],
  [-1, "-1"],
  [1n, "1n"],
  ["0", "the string \"0\""],
  ["false", "the string \"false\""],
  [{}, "an object"],
  [[], "an empty array"],
  [Symbol(), "a Symbol"],
  [() => {}, "a function"],
  [Infinity, "Infinity"],
]) {
  promise_test(async () => {
    assert_equals(roundtrip(input), true);
    assert_equals(received, true, "value seen by echo");
  }, `bool: ${description} is true under ToBoolean`);
}

promise_test(async () => {
  let calls = 0;
  const value = { valueOf() { calls++; return 0; }, toString() { calls++; return ""; } };
  assert_equals(roundtrip(value), true);
  assert_equals(calls, 0, "ToBoolean does not call valueOf or toString");
}, "bool: ToBoolean never invokes valueOf or toString");

promise_test(async () => {
  const { roundtrip } = await componentExports("values/bool", { echo: () => "yes", recv() {} });
  assert_equals(roundtrip(false), true);
}, "bool: an import return value goes through ToBoolean");

promise_test(async () => {
  const { roundtrip } = await componentExports("values/bool", { echo: () => undefined, recv() {} });
  assert_equals(roundtrip(true), false);
}, "bool: an import returning undefined yields false");

promise_test(async () => {
  spied.length = 0;
  send();
  assert_array_equals(spied, [false, true, true, true]);
  for (const value of spied) {
    assert_equals(typeof value, "boolean");
  }
}, "bool: lifting treats every non-zero i32 as true");
