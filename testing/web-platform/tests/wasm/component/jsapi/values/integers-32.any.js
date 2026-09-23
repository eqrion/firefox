// META: global=window,dedicatedworker,jsshell
// META: script=/wasm/component/jsapi/harness/component-helpers.js
// META: script=/wasm/component/jsapi/harness/assertions.js

// Conversions for the integer types that flatten to i32. u32 has its own
// file. The fixture exports roundtrip-<type> calling echo-<type>.

const types = {
  s8: { min: -0x80, max: 0x7f },
  s16: { min: -0x8000, max: 0x7fff },
  s32: { min: -0x80000000, max: 0x7fffffff },
  u8: { min: 0, max: 0xff },
  u16: { min: 0, max: 0xffff },
};

const imports = {};
const received = {};
for (const type of Object.keys(types)) {
  imports[`echo${type[0].toUpperCase()}${type.slice(1)}`] = x => { received[type] = x; return x; };
}

let exports;
promise_setup(async () => {
  exports = await componentExports("values/integers-32", imports);
});

function roundtrip(type, value) {
  return exports[`roundtrip${type[0].toUpperCase()}${type.slice(1)}`](value);
}

for (const [type, { min, max }] of Object.entries(types)) {
  for (const value of [min, max, 0, 1, min + 1, max - 1]) {
    promise_test(async () => {
      assert_equals(roundtrip(type, value), value);
      assert_equals(received[type], value, "value seen by echo");
    }, `${type}: ${value} roundtrips exactly`);
  }

  for (const [input, expected, description] of [
    [1.9, 1, "1.9 truncates toward zero"],
    [-0.5, 0, "-0.5 truncates toward zero"],
    [-0, 0, "-0 becomes 0"],
    ["1", 1, "a numeric string goes through ToNumber"],
    [" 1 ", 1, "whitespace around a numeric string is accepted"],
    ["0x1", 1, "a hex string goes through ToNumber"],
    ["", 0, "the empty string is 0"],
    [true, 1, "true is 1"],
    [false, 0, "false is 0"],
    [null, 0, "null is 0"],
    [[], 0, "an empty array is 0"],
    [[1], 1, "a one-element array is its element"],
    [{ valueOf() { return 1; } }, 1, "valueOf is honoured"],
    [`${max}`, max, "the maximum as a string"],
  ]) {
    promise_test(async () => {
      assert_equals(roundtrip(type, input), expected);
      assert_equals(received[type], expected, "value seen by echo");
    }, `${type}: ${description}`);
  }

  if (min < 0) {
    promise_test(async () => {
      assert_equals(roundtrip(type, -1.9), -1);
      assert_equals(roundtrip(type, `${min}`), min);
    }, `${type}: negative values truncate toward zero and parse from strings`);
  }

  for (const [input, description] of [
    [min - 1, `${min - 1} is below range`],
    [max + 1, `${max + 1} is above range`],
    [min - 1.5, `${min - 1.5} truncates below range`],
    [max + 1.5, `${max + 1.5} truncates above range`],
    [NaN, "NaN"],
    [Infinity, "Infinity"],
    [-Infinity, "-Infinity"],
    [undefined, "undefined"],
    ["abc", "a non-numeric string"],
    [1n, "a BigInt"],
    [Symbol(), "a Symbol"],
    [{}, "a plain object"],
    [[1, 2], "a two-element array"],
  ]) {
    promise_test(async () => {
      received[type] = "unchanged";
      assert_throws_js(TypeError, () => roundtrip(type, input));
      assert_equals(received[type], "unchanged", "echo must not be called");
    }, `${type}: ${description} throws TypeError`);
  }

  promise_test(async () => {
    assert_equals(roundtrip(type, max + 0.9), max, "just below the range edge truncates in");
    if (min < 0) {
      assert_equals(roundtrip(type, min - 0.9), min, "just below the negative edge truncates in");
    } else {
      assert_equals(roundtrip(type, -0.9), 0, "-0.9 truncates to 0");
    }
  }, `${type}: fractional values inside the range after truncation are accepted`);

  promise_test(async () => {
    let calls = 0;
    roundtrip(type, { valueOf() { calls++; return 1; } });
    assert_equals(calls, 1);
  }, `${type}: valueOf is called exactly once`);

  promise_test(async () => {
    const echo = { ...imports, [`echo${type[0].toUpperCase()}${type.slice(1)}`]: () => max + 1 };
    const e = await componentExports("values/integers-32", echo);
    assert_traps(() => e[`roundtrip${type[0].toUpperCase()}${type.slice(1)}`](0));
  }, `${type}: an import returning an out-of-range value traps`);

  promise_test(async () => {
    const echo = { ...imports, [`echo${type[0].toUpperCase()}${type.slice(1)}`]: () => `${max}` };
    const e = await componentExports("values/integers-32", echo);
    assert_equals(e[`roundtrip${type[0].toUpperCase()}${type.slice(1)}`](0), max);
  }, `${type}: an import return value is coerced like an argument`);

  promise_test(async () => {
    const echo = { ...imports, [`echo${type[0].toUpperCase()}${type.slice(1)}`]: () => 1n };
    const e = await componentExports("values/integers-32", echo);
    assert_traps(() => e[`roundtrip${type[0].toUpperCase()}${type.slice(1)}`](0));
  }, `${type}: an import returning a BigInt traps`);
}
