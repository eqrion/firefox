// META: global=window,dedicatedworker,jsshell
// META: script=/wasm/component/jsapi/harness/component-helpers.js
// META: script=/wasm/component/jsapi/harness/assertions.js

// s64 and u64 convert to BigInt with ToBigInt64 and ToBigUint64, which wrap
// and require a BigInt, a boolean or a string.

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
    ["12", 12n, "a numeric string goes through StringToBigInt"],
    ["0x10", 16n, "a hex string goes through StringToBigInt"],
    ["", 0n, "the empty string is 0n"],
    [true, 1n, "true is 1n"],
    [false, 0n, "false is 0n"],
    [[], 0n, "an empty array is 0n, through its string"],
    [{ valueOf() { return 7n; } }, 7n, "valueOf returning a BigInt is honoured"],
    [min - 1n, max, `${min - 1n}n below range wraps`],
    [max + 1n, min, `${max + 1n}n above range wraps`],
    [max + 2n ** 64n + 1n, min, "a BigInt several times the range away wraps"],
  ]) {
    promise_test(async () => {
      assert_bigint_equals(roundtrip(type, input), expected, "result");
      assert_bigint_equals(received[type], expected, "value seen by echo");
    }, `${type}: ${description}`);
  }

  promise_test(async () => {
    received[type] = "unchanged";
    assert_throws_js(SyntaxError, () => roundtrip(type, "abc"));
    assert_throws_js(SyntaxError, () => roundtrip(type, "1n"));
    assert_throws_js(SyntaxError, () => roundtrip(type, {}));
    assert_throws_js(SyntaxError, () => roundtrip(type, "1.5"));
    assert_equals(received[type], "unchanged", "echo must not be called");
  }, `${type}: a string that is not an integer throws SyntaxError`);

  for (const [input, description] of [
    [0, "the Number 0"],
    [1, "the Number 1"],
    [2 ** 53, "2**53 as a Number"],
    [NaN, "NaN"],
    [Infinity, "Infinity"],
    [undefined, "undefined"],
    [null, "null"],
    [{ valueOf() { return 7; } }, "an object whose valueOf returns a Number"],
    [Symbol(), "a Symbol"],
  ]) {
    promise_test(async () => {
      received[type] = "unchanged";
      assert_throws_js(TypeError, () => roundtrip(type, input));
      assert_equals(received[type], "unchanged", "echo must not be called");
    }, `${type}: ${description} throws TypeError`);
  }

  promise_test(async () => {
    let calls = 0;
    roundtrip(type, { valueOf() { calls++; return 1n; } });
    assert_equals(calls, 1);
  }, `${type}: valueOf is called exactly once`);
}

promise_test(async () => {
  assert_bigint_equals(roundtrip("u64", -1n), 2n ** 64n - 1n, "-1n");
  assert_bigint_equals(roundtrip("u64", "-1"), 2n ** 64n - 1n, "\"-1\"");
}, "u64: negative values wrap");

promise_test(async () => {
  assert_bigint_equals(roundtrip("s64", 2n ** 63n), -(2n ** 63n), "2n**63n");
  assert_bigint_equals(roundtrip("s64", 2n ** 64n - 1n), -1n, "2n**64n - 1n");
}, "s64: values above the range wrap negative");

promise_test(async () => {
  const e = await componentExports("values/integers-64", { ...imports, echoS64: () => 5 });
  assert_traps(() => e.roundtripS64(0n));
}, "s64: an import returning a Number traps");

promise_test(async () => {
  const e = await componentExports("values/integers-64", { ...imports, echoU64: () => 2n ** 64n });
  assert_bigint_equals(e.roundtripU64(0n), 0n, "result");
}, "u64: an import returning an out-of-range BigInt wraps");

promise_test(async () => {
  const e = await componentExports("values/integers-64", { ...imports, echoS64: () => "x" });
  assert_traps(() => e.roundtripS64(0n));
}, "s64: an import returning a non-numeric string traps");
