// META: global=window,dedicatedworker,jsshell
// META: script=/wasm/component/jsapi/harness/component-helpers.js
// META: script=/wasm/component/jsapi/harness/assertions.js

const received = {};
const spied = { f32: [], f64: [] };
const imports = {
  echoF32: x => { received.f32 = x; return x; },
  echoF64: x => { received.f64 = x; return x; },
  recvF32: x => { spied.f32.push(x); },
  recvF64: x => { spied.f64.push(x); },
};

let exports;
promise_setup(async () => {
  exports = await componentExports("values/floats", imports);
});

function assert_same_number(actual, expected, description) {
  assert_equals(typeof actual, "number", `${description}: typeof`);
  assert_true(Object.is(actual, expected),
              `${description}: expected ${format_value(expected)}, got ${format_value(actual)}`);
}

const f64Values = [0, -0, 1, -1, 1.5, Math.PI, 1e300, -1e-300, Number.MAX_VALUE, Number.MIN_VALUE,
                   Number.MAX_SAFE_INTEGER, Infinity, -Infinity];

for (const value of f64Values) {
  promise_test(async () => {
    assert_same_number(exports.roundtripF64(value), value, "result");
    assert_same_number(received.f64, value, "value seen by echo");
  }, `f64: ${format_value(value)} roundtrips exactly`);
}

promise_test(async () => {
  assert_true(Number.isNaN(exports.roundtripF64(NaN)), "result");
  assert_true(Number.isNaN(received.f64), "value seen by echo");
}, "f64: NaN roundtrips as NaN");

const f32Exact = [0, -0, 1, -1, 1.5, 0.25, 3.4028234663852886e38, 1.1754943508222875e-38, 2 ** -140,
                  Infinity, -Infinity];
for (const value of f32Exact) {
  promise_test(async () => {
    assert_equals(Math.fround(value), value, "test value is representable as f32");
    assert_same_number(exports.roundtripF32(value), value, "result");
    assert_same_number(received.f32, value, "value seen by echo");
  }, `f32: ${format_value(value)} roundtrips exactly`);
}

for (const [value, description] of [
  [0.1, "0.1"],
  [1 / 3, "1/3"],
  [16777217, "2**24 + 1 (ties to even, rounds down)"],
  [16777219, "2**24 + 3 (ties to even, rounds up)"],
  [1 + 2 ** -24, "1 + 2**-24 (ties to even, rounds down)"],
  [1 + 3 * 2 ** -24, "1 + 3 * 2**-24 (ties to even, rounds up)"],
  [Math.PI, "pi"],
  [1e-46, "1e-46 (underflows to 0)"],
  [-1e-46, "-1e-46 (underflows to -0)"],
  [1e-39, "1e-39 (denormal)"],
  [3.4028235677973366e38, "halfway between f32 max and 2**128 (rounds to Infinity)"],
  [2 ** 128, "2**128 (overflows to Infinity)"],
  [-(2 ** 128), "-(2**128) (overflows to -Infinity)"],
  [Number.MAX_VALUE, "Number.MAX_VALUE (overflows to Infinity)"],
]) {
  promise_test(async () => {
    const expected = Math.fround(value);
    assert_same_number(exports.roundtripF32(value), expected, "result");
    assert_same_number(received.f32, expected, "value seen by echo");
  }, `f32: ${description} rounds to nearest, ties to even`);
}

promise_test(async () => {
  assert_true(Number.isNaN(exports.roundtripF32(NaN)), "result");
  assert_true(Number.isNaN(received.f32), "value seen by echo");
}, "f32: NaN roundtrips as NaN");

for (const type of ["f32", "f64"]) {
  const roundtrip = type === "f32" ? v => exports.roundtripF32(v) : v => exports.roundtripF64(v);

  for (const [input, expected, description] of [
    ["1.5", 1.5, "a numeric string goes through ToNumber"],
    ["  -2.5\n", -2.5, "whitespace around a numeric string is accepted"],
    ["0x10", 16, "a hex string goes through ToNumber"],
    ["Infinity", Infinity, "the string \"Infinity\""],
    ["", 0, "the empty string is 0"],
    [true, 1, "true is 1"],
    [false, 0, "false is 0"],
    [null, 0, "null is 0"],
    [[], 0, "an empty array is 0"],
    [["2.5"], 2.5, "a one-element array is its element"],
    [{ valueOf() { return 0.5; } }, 0.5, "valueOf is honoured"],
  ]) {
    promise_test(async () => {
      assert_same_number(roundtrip(input), expected, "result");
      assert_same_number(received[type], expected, "value seen by echo");
    }, `${type}: ${description}`);
  }

  for (const [input, description] of [
    [undefined, "undefined"],
    ["abc", "a non-numeric string"],
    [{}, "a plain object"],
    [[1, 2], "a two-element array"],
  ]) {
    promise_test(async () => {
      assert_true(Number.isNaN(roundtrip(input)), "result");
      assert_true(Number.isNaN(received[type]), "value seen by echo");
    }, `${type}: ${description} converts to NaN and is accepted`);
  }

  for (const [input, description] of [
    [1n, "a BigInt"],
    [Symbol(), "a Symbol"],
  ]) {
    promise_test(async () => {
      received[type] = "unchanged";
      assert_throws_js(TypeError, () => roundtrip(input));
      assert_equals(received[type], "unchanged", "echo must not be called");
    }, `${type}: ${description} throws TypeError`);
  }

  promise_test(async () => {
    let calls = 0;
    roundtrip({ valueOf() { calls++; return 1; } });
    assert_equals(calls, 1);
  }, `${type}: valueOf is called exactly once`);
}

promise_test(async () => {
  const e = await componentExports("values/floats", { ...imports, echoF64: () => "2.5" });
  assert_same_number(e.roundtripF64(0), 2.5, "result");
}, "f64: an import return value goes through ToNumber");

promise_test(async () => {
  const e = await componentExports("values/floats", { ...imports, echoF32: () => 0.1 });
  assert_same_number(e.roundtripF32(0), Math.fround(0.1), "result");
}, "f32: an import return value is rounded to f32");

promise_test(async () => {
  const e = await componentExports("values/floats", { ...imports, echoF64: () => 1n });
  assert_traps(() => e.roundtripF64(0));
}, "f64: an import returning a BigInt traps");

promise_test(async () => {
  const e = await componentExports("values/floats", { ...imports, echoF32: () => undefined });
  assert_true(Number.isNaN(e.roundtripF32(0)));
}, "f32: an import returning undefined yields NaN");

promise_test(async () => {
  spied.f32.length = 0;
  spied.f64.length = 0;
  exports.send();
  assert_equals(spied.f32.length, 7);
  assert_equals(spied.f64.length, 7);
  for (const list of [spied.f32, spied.f64]) {
    assert_true(Number.isNaN(list[0]), "NaN with a payload lifts to NaN");
    assert_true(Number.isNaN(list[1]), "negative NaN with a payload lifts to NaN");
    assert_same_number(list[2], Infinity, "Infinity");
    assert_same_number(list[3], -Infinity, "-Infinity");
    assert_same_number(list[4], -0, "-0");
  }
  assert_same_number(spied.f32[5], 1.401298464324817e-45, "smallest f32 denormal");
  assert_same_number(spied.f32[6], 3.4028234663852886e38, "largest finite f32");
  assert_same_number(spied.f64[5], 5e-324, "smallest f64 denormal");
  assert_same_number(spied.f64[6], Number.MAX_VALUE, "largest finite f64");
}, "floats: guest-produced specials lift to the matching JS Numbers");
