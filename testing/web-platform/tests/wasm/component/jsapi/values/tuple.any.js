// META: global=window,dedicatedworker,jsshell
// META: script=/wasm/component/jsapi/harness/component-helpers.js
// META: script=/wasm/component/jsapi/harness/assertions.js

// tuple<u32, string, bool> converts like a list with exactly 3 elements,
// each converted as its own type.

let roundtrip;
let received;
promise_setup(async () => {
  const echoMixed = x => { received = x; return x; };
  ({ roundtripMixed: roundtrip } = await componentExports("values/tuple", { echoMixed }));
});

promise_test(async () => {
  const input = [1, "a", true];
  const result = roundtrip(input);
  assert_true(Array.isArray(result));
  assert_equals(Object.getPrototypeOf(result), Array.prototype);
  assert_equals(result.length, 3);
  assert_value_equals(result, input, "result");
  assert_value_equals(received, input, "value seen by echo");
  assert_not_equals(result, input, "result is a fresh Array");
}, "tuple: an Array of the right arity roundtrips as an Array");

promise_test(async () => {
  assert_value_equals(roundtrip(["7", 3, 0]), [7, "3", false], "result");
  assert_value_equals(roundtrip([1.9, null, "x"]), [1, "null", true], "result");
}, "tuple: each element is converted as its own type");

promise_test(async () => {
  assert_value_equals(roundtrip(new Set([2, "b", false])), [2, "b", false], "Set");
  assert_value_equals(roundtrip((function* () { yield 3; yield "c"; yield 1; })()), [3, "c", true],
                      "generator");
}, "tuple: any iterable of the right arity converts");

for (const [input, description] of [
  [[], "an empty Array"],
  [[1, "a"], "2 elements"],
  [[1, "a", true, 0], "4 elements"],
  [[1n, "a", true], "a BigInt first element"],
  [[1, Symbol(), true], "a Symbol second element"],
  ["abc", "a string"],
  [{ 0: 1, 1: "a", 2: true, length: 3 }, "a non-iterable array-like"],
  [null, "null"],
  [undefined, "undefined"],
]) {
  promise_test(async () => {
    received = "unchanged";
    assert_throws_js(TypeError, () => roundtrip(input));
    assert_equals(received, "unchanged", "echo must not be called");
  }, `tuple: ${description} throws TypeError`);
}

promise_test(async () => {
  const { roundtripMixed } = await componentExports("values/tuple", { echoMixed: () => ["9", 9, 9] });
  assert_value_equals(roundtripMixed([1, "a", true]), [9, "9", true], "result");
}, "tuple: an import return value is converted element-wise");

for (const [value, description] of [
  [[1, "a"], "2 elements"],
  [[1, "a", true, 0], "4 elements"],
  [5, "a Number"],
  [[1n, "a", true], "a BigInt element"],
]) {
  promise_test(async () => {
    const { roundtripMixed } = await componentExports("values/tuple", { echoMixed: () => value });
    assert_traps(() => roundtripMixed([1, "a", true]));
  }, `tuple: an import returning ${description} traps`);
}
