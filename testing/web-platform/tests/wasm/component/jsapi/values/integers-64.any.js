// META: global=window,dedicatedworker,jsshell
// META: script=/wasm/component/jsapi/harness/component-helpers.js
// META: script=/wasm/component/jsapi/harness/assertions.js

// s64 and u64 convert to BigInt and accept BigInt or Number.

const types = {
  s64: { min: -(2n ** 63n), max: 2n ** 63n - 1n },
  u64: { min: 0n, max: 2n ** 64n - 1n },
};

const received = {};
const imports = {
  echoS64: x => { received.s64 = x; return x; },
  echoU64: x => { received.u64 = x; return x; },
};

let exports;
promise_setup(async () => {
  exports = await componentExports("values/integers-64", imports);
});

function roundtrip(type, value) {
  return exports[type === "s64" ? "roundtripS64" : "roundtripU64"](value);
}

function assert_bigint_equals(actual, expected, description) {
  assert_equals(typeof actual, "bigint", `${description}: typeof`);
  assert_equals(actual, expected, description);
}

for (const [type, { min, max }] of Object.entries(types)) {
  for (const value of new Set([min, max, 0n, 1n, min + 1n, max - 1n, 2n ** 53n, 2n ** 53n + 1n])) {
    promise_test(async () => {
      assert_bigint_equals(roundtrip(type, value), value, "result");
      assert_bigint_equals(received[type], value, "value seen by echo");
    }, `${type}: ${value}n roundtrips exactly`);
  }

  for (const [input, expected, description] of [
    [0, 0n, "the Number 0"],
    [1, 1n, "the Number 1"],
    [1.5, 1n, "1.5 truncates toward zero"],
    [-0.5, 0n, "-0.5 truncates toward zero"],
    [-0, 0n, "-0 becomes 0n"],
    [2 ** 53, 2n ** 53n, "2**53 as a Number"],
    ["12", 12n, "a numeric string goes through ToNumber"],
    ["0x10", 16n, "a hex string goes through ToNumber"],
    [true, 1n, "true is 1n"],
    [false, 0n, "false is 0n"],
    [null, 0n, "null is 0n"],
    [[], 0n, "an empty array is 0n"],
    [{ valueOf() { return 7; } }, 7n, "valueOf returning a Number is honoured"],
  ]) {
    promise_test(async () => {
      assert_bigint_equals(roundtrip(type, input), expected, "result");
      assert_bigint_equals(received[type], expected, "value seen by echo");
    }, `${type}: ${description}`);
  }

  for (const [input, description] of [
    [min - 1n, `${min - 1n}n is below range`],
    [max + 1n, `${max + 1n}n is above range`],
    [NaN, "NaN"],
    [Infinity, "Infinity"],
    [-Infinity, "-Infinity"],
    [undefined, "undefined"],
    ["abc", "a non-numeric string"],
    ["1n", "a BigInt-looking string"],
    [Symbol(), "a Symbol"],
    [{}, "a plain object"],
    [2 ** 64, "2**64 as a Number"],
    [{ valueOf() { return 7n; } }, "an object whose valueOf returns a BigInt (ToNumber throws)"],
  ]) {
    promise_test(async () => {
      received[type] = "unchanged";
      assert_throws_js(TypeError, () => roundtrip(type, input));
      assert_equals(received[type], "unchanged", "echo must not be called");
    }, `${type}: ${description} throws TypeError`);
  }

  promise_test(async () => {
    let calls = 0;
    roundtrip(type, { valueOf() { calls++; return 1; } });
    assert_equals(calls, 1);
  }, `${type}: valueOf is called exactly once`);
}

promise_test(async () => {
  assert_bigint_equals(roundtrip("s64", -1), -1n, "result");
  assert_bigint_equals(roundtrip("s64", -1.9), -1n, "truncated");
  assert_bigint_equals(roundtrip("s64", -(2 ** 63)), -(2n ** 63n), "minimum as a Number");
}, "s64: negative Numbers are accepted");

promise_test(async () => {
  assert_throws_js(TypeError, () => roundtrip("u64", -1));
  assert_throws_js(TypeError, () => roundtrip("u64", -1n));
  assert_throws_js(TypeError, () => roundtrip("u64", "-1"));
}, "u64: negative values throw TypeError");

promise_test(async () => {
  assert_bigint_equals(roundtrip("u64", 2 ** 63), 2n ** 63n, "2**63 as a Number");
  assert_throws_js(TypeError, () => roundtrip("s64", 2 ** 63));
}, "Number 2**63 fits u64 but not s64");

promise_test(async () => {
  const e = await componentExports("values/integers-64", { ...imports, echoS64: () => 5 });
  assert_bigint_equals(e.roundtripS64(0n), 5n, "result");
}, "s64: an import returning a Number is converted to BigInt");

promise_test(async () => {
  const e = await componentExports("values/integers-64", { ...imports, echoU64: () => 2n ** 64n });
  assert_traps(() => e.roundtripU64(0n));
}, "u64: an import returning an out-of-range BigInt traps");

promise_test(async () => {
  const e = await componentExports("values/integers-64", { ...imports, echoS64: () => "x" });
  assert_traps(() => e.roundtripS64(0n));
}, "s64: an import returning a non-numeric string traps");
