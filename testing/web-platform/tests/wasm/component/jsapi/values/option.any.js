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
  ["roundtripU32", null, undefined, "null is none"],
  ["roundtripU32", undefined, undefined, "undefined is none"],
  ["roundtripU32", 5, 5, "a number is some"],
  ["roundtripU32", 0, 0, "0 is some(0), not none"],
  ["roundtripU32", "7", 7, "a numeric string is some(7)"],
  ["roundtripString", null, undefined, "null is none"],
  ["roundtripString", "", "", "the empty string is some(\"\")"],
  ["roundtripString", "x", "x", "a string is some"],
  ["roundtripString", 42, "42", "a number is some(\"42\")"],
  ["roundtripString", false, "false", "false is some(\"false\")"],
  ["roundtripBool", null, undefined, "null is none"],
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

for (const [name, input, expected, description] of [
  ["roundtripU32", NaN, 0, "a NaN payload is some(0)"],
  ["roundtripU32", -1, 0xffffffff, "an out-of-range payload wraps"],
  ["roundtripU32", {}, 0, "an object payload is some(0)"],
]) {
  promise_test(async () => {
    const result = exports[name](input);
    assert_value_equals(received, expected, "value seen by echo");
    assert_value_equals(result, expected);
  }, `option: ${name}: ${description}`);
}

for (const [name, input, description] of [
  ["roundtripU32", 1n, "BigInt payload"],
  ["roundtripString", Symbol(), "Symbol payload"],
]) {
  promise_test(async () => {
    received = "unchanged";
    assert_throws_js(TypeError, () => exports[name](input));
    assert_equals(received, "unchanged", "echo must not be called");
  }, `option: ${name}: ${description} throws TypeError`);
}

for (const [input, expected, description] of [
  [null, undefined, "null is none"],
  [undefined, undefined, "undefined is none"],
  [variant("none"), variant("none"), "some(none)"],
  [{ kind: "none" }, variant("none"), "some(none) from an ordinary object"],
  [{ kind: "none", value: 5 }, variant("none"), "some(none) ignores a value"],
  [{ kind: "some", value: 3 }, variant("some", 3), "some(some(3))"],
  [{ kind: "some", value: 0 }, variant("some", 0), "some(some(0))"],
  [{ kind: "some", value: "x" }, variant("some", 0), "some(some(0)) via a payload that converts to 0"],
]) {
  promise_test(async () => {
    const result = exports.roundtripNested(input);
    assert_value_equals(received, expected, "value seen by echo");
    assert_value_equals(result, expected);
  }, `option<option<u32>>: ${description}`);
}

promise_test(async () => {
  assert_array_equals(Reflect.ownKeys(exports.roundtripNested({ kind: "none" })), ["kind"]);
  assert_array_equals(Reflect.ownKeys(exports.roundtripNested({ kind: "some", value: 1 })), ["kind", "value"]);
}, "option<option<u32>>: the inner none has no value property, some does");

for (const [input, description] of [
  [3, "a bare number"],
  ["some", "a bare string"],
  [{}, "an object without kind"],
  [{ kind: "None" }, "a PascalCase kind"],
  [{ kind: "Some", value: 1 }, "a PascalCase Some"],
  [{ kind: "other" }, "an unknown kind"],
  [{ kind: "some", value: 1n }, "some with an unconvertible payload"],
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
  assert_equals(roundtripU32(1), undefined);
}, "option: an import returning undefined yields none");

promise_test(async () => {
  const { roundtripNested } = await componentExports("values/option", {
    echoU32: x => x, echoString: x => x, echoNested: () => null, echoBool: x => x,
  });
  assert_equals(roundtripNested({ kind: "none" }), undefined);
}, "option<option<u32>>: an import returning null yields none");

promise_test(async () => {
  const { roundtripNested } = await componentExports("values/option", {
    echoU32: x => x, echoString: x => x, echoNested: () => 1, echoBool: x => x,
  });
  assert_traps(() => roundtripNested({ kind: "none" }));
}, "option<option<u32>>: an import returning a bare payload traps");
