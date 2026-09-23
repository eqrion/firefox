// META: global=window,dedicatedworker,jsshell
// META: script=/wasm/component/jsapi/harness/component-helpers.js
// META: script=/wasm/component/jsapi/harness/assertions.js

// Conversions for a flat integer type, using the passthrough fixture:
// roundtrip(x) lowers x, calls echo(x) and lifts echo's return value.

let roundtrip;
let received;
promise_setup(async () => {
  const echo = x => { received = x; return x; };
  ({ roundtrip } = await componentExports("values/u32", { echo }));
});

for (const value of [0, 1, 42, 0x7fffffff, 0x80000000, 0xffffffff]) {
  promise_test(async () => {
    assert_equals(roundtrip(value), value);
    assert_value_equals(received, value, "value seen by echo");
  }, `u32: ${value} roundtrips exactly`);
}

for (const [input, expected, description] of [
  [1.9, 1, "truncates toward zero"],
  [-0.5, 0, "truncates -0.5 toward zero"],
  [-0, 0, "-0 becomes 0"],
  ["42", 42, "strings go through ToNumber"],
  [" 7 ", 7, "whitespace around a numeric string is accepted"],
  ["0x10", 16, "hex string goes through ToNumber"],
  [true, 1, "true is 1"],
  [false, 0, "false is 0"],
  [null, 0, "null is 0"],
  [[], 0, "empty array is 0"],
  [{ valueOf() { return 5; } }, 5, "valueOf is honoured"],
]) {
  promise_test(async () => {
    assert_equals(roundtrip(input), expected);
    assert_value_equals(received, expected, "value seen by echo");
  }, `u32: ${description}`);
}

for (const [input, description] of [
  [-1, "-1 is below range"],
  [0x100000000, "2**32 is above range"],
  [NaN, "NaN"],
  [Infinity, "Infinity"],
  [-Infinity, "-Infinity"],
  [undefined, "undefined"],
  ["abc", "non-numeric string"],
  [1n, "BigInt"],
  [Symbol(), "Symbol"],
  [{}, "plain object"],
]) {
  promise_test(async () => {
    received = "unchanged";
    assert_throws_js(TypeError, () => roundtrip(input));
    assert_equals(received, "unchanged", "echo must not be called");
  }, `u32: ${description} throws TypeError`);
}

promise_test(async () => {
  let calls = 0;
  roundtrip({ valueOf() { calls++; return 3; } });
  assert_equals(calls, 1);
}, "u32: valueOf is called exactly once");

promise_test(async () => {
  const { roundtrip } = await componentExports("values/u32", { echo: () => -1 });
  assert_traps(() => roundtrip(1));
}, "u32: an import returning an out-of-range value traps");

promise_test(async () => {
  const { roundtrip } = await componentExports("values/u32", { echo: () => "9" });
  assert_equals(roundtrip(1), 9);
}, "u32: an import return value is coerced like an argument");
