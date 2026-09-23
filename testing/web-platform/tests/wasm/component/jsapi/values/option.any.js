// META: global=window,dedicatedworker,jsshell
// META: script=/wasm/component/jsapi/harness/component-helpers.js
// META: script=/wasm/component/jsapi/harness/assertions.js

// option<u32>, option<string>, option<option<u32>>, option<bool>

let exports;
let received;
promise_setup(async () => {
  const echo = x => { received = x; return x; };
  exports = await componentExports("values/option", {
    echoU32: echo, echoString: echo, echoNested: echo, echoBool: echo,
  });
});

for (const [name, input, expected, description] of [
  ["roundtripU32", null, null, "null is none"],
  ["roundtripU32", undefined, null, "undefined is none"],
  ["roundtripU32", 5, 5, "a number is some"],
  ["roundtripU32", 0, 0, "0 is some(0), not none"],
  ["roundtripU32", "7", 7, "a numeric string is some(7)"],
  ["roundtripString", null, null, "null is none"],
  ["roundtripString", "", "", "the empty string is some(\"\")"],
  ["roundtripString", "x", "x", "a string is some"],
  ["roundtripString", 42, "42", "a number is some(\"42\")"],
  ["roundtripString", false, "false", "false is some(\"false\")"],
  ["roundtripBool", null, null, "null is none"],
  ["roundtripBool", false, false, "false is some(false), not none"],
  ["roundtripBool", 0, false, "0 is some(false)"],
  ["roundtripBool", "", false, "the empty string is some(false)"],
  ["roundtripBool", NaN, false, "NaN is some(false)"],
  ["roundtripBool", {}, true, "an object is some(true)"],
]) {
  promise_test(async () => {
    const result = exports[name](input);
    assert_value_equals(received, expected, "value seen by echo");
    assert_value_equals(result, expected);
  }, `option: ${name}: ${description}`);
}

for (const [name, input, description] of [
  ["roundtripU32", NaN, "NaN payload"],
  ["roundtripU32", -1, "out-of-range payload"],
  ["roundtripU32", {}, "object payload"],
  ["roundtripString", Symbol(), "Symbol payload"],
]) {
  promise_test(async () => {
    received = "unchanged";
    assert_throws_js(TypeError, () => exports[name](input));
    assert_equals(received, "unchanged", "echo must not be called");
  }, `option: ${name}: ${description} throws TypeError`);
}

for (const [input, expected, description] of [
  [variant("None"), variant("None"), "None"],
  [{ kind: "None" }, variant("None"), "None from an ordinary object"],
  [{ kind: "None", value: 5 }, variant("None"), "None ignores a value"],
  [{ kind: "Some", value: 3 }, variant("Some", 3), "Some(some(3))"],
  [{ kind: "Some", value: null }, variant("Some", null), "Some(none) via null"],
  [{ kind: "Some", value: undefined }, variant("Some", null), "Some(none) via undefined"],
  [{ kind: "Some" }, variant("Some", null), "Some(none) via a missing value"],
  [{ kind: "Some", value: 0 }, variant("Some", 0), "Some(some(0))"],
]) {
  promise_test(async () => {
    const result = exports.roundtripNested(input);
    assert_value_equals(received, expected, "value seen by echo");
    assert_value_equals(result, expected);
    assert_equals(Object.getPrototypeOf(result), null);
  }, `option<option<u32>>: ${description}`);
}

promise_test(async () => {
  assert_array_equals(Reflect.ownKeys(exports.roundtripNested({ kind: "None" })), ["kind"]);
  assert_array_equals(Reflect.ownKeys(exports.roundtripNested({ kind: "Some", value: 1 })), ["kind", "value"]);
}, "option<option<u32>>: None has no value property, Some does");

for (const [input, description] of [
  [null, "null"],
  [undefined, "undefined"],
  [3, "a bare number"],
  ["Some", "a bare string"],
  [{}, "an object without kind"],
  [{ kind: "none" }, "a lower-case kind"],
  [{ kind: "some", value: 1 }, "a lower-case Some"],
  [{ kind: "Other" }, "an unknown kind"],
  [{ kind: "Some", value: "x" }, "Some with an unconvertible payload"],
]) {
  promise_test(async () => {
    received = "unchanged";
    assert_throws_js(TypeError, () => exports.roundtripNested(input));
    assert_equals(received, "unchanged", "echo must not be called");
  }, `option<option<u32>>: ${description} throws TypeError`);
}

promise_test(async () => {
  const { roundtripU32 } = await componentExports("values/option", {
    echoU32: () => undefined, echoString: x => x, echoNested: x => x, echoBool: x => x,
  });
  assert_equals(roundtripU32(1), null);
}, "option: an import returning undefined yields none");

promise_test(async () => {
  const { roundtripNested } = await componentExports("values/option", {
    echoU32: x => x, echoString: x => x, echoNested: () => null, echoBool: x => x,
  });
  assert_traps(() => roundtripNested({ kind: "None" }));
}, "option<option<u32>>: an import returning null traps");
