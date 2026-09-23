// META: global=window,dedicatedworker,jsshell
// META: script=/wasm/component/jsapi/harness/component-helpers.js
// META: script=/wasm/component/jsapi/harness/assertions.js

let exports;
let received;
const spied = [];
promise_setup(async () => {
  const echo = x => { received = x; return x; };
  const recv = x => { spied.push(x); };
  exports = await componentExports("values/char", { echo, recv });
});

for (const [value, description] of [
  ["a", "ASCII"],
  ["\0", "U+0000"],
  ["é", "Latin-1"],
  ["퟿", "U+D7FF (just below the surrogates)"],
  ["", "U+E000 (just above the surrogates)"],
  ["￿", "U+FFFF"],
  ["\u{10000}", "U+10000 (first astral)"],
  ["😀", "U+1F600 (surrogate pair)"],
  ["\u{10FFFF}", "U+10FFFF (last scalar value)"],
]) {
  promise_test(async () => {
    const result = exports.roundtrip(value);
    assert_equals(result, value);
    assert_equals(typeof result, "string");
    assert_equals(received, value, "value seen by echo");
  }, `char: ${description} roundtrips exactly`);
}

for (const [input, expected, description] of [
  [7, "7", "a single-digit Number"],
  [{ toString() { return "x"; } }, "x", "an object with toString"],
  [["y"], "y", "a one-element array"],
]) {
  promise_test(async () => {
    assert_equals(exports.roundtrip(input), expected);
    assert_equals(received, expected, "value seen by echo");
  }, `char: ${description} is converted with ToString`);
}

for (const [input, description] of [
  ["", "the empty string"],
  ["ab", "two characters"],
  ["\uD800", "a lone lead surrogate"],
  ["\uDC00", "a lone trail surrogate"],
  ["\uDC00\uD800", "a reversed surrogate pair"],
  ["😀a", "a surrogate pair plus a character"],
  [42, "a two-digit Number"],
  [true, "a boolean (\"true\")"],
  [null, "null (\"null\")"],
  [undefined, "undefined (\"undefined\")"],
  [Symbol(), "a Symbol"],
  [{}, "a plain object (\"[object Object]\")"],
]) {
  promise_test(async () => {
    received = "unchanged";
    assert_throws_js(TypeError, () => exports.roundtrip(input));
    assert_equals(received, "unchanged", "echo must not be called");
  }, `char: ${description} throws TypeError`);
}

promise_test(async () => {
  let calls = 0;
  exports.roundtrip({ toString() { calls++; return "z"; } });
  assert_equals(calls, 1);
}, "char: toString is called exactly once");

promise_test(async () => {
  const { roundtrip } = await componentExports("values/char", { echo: () => 5, recv() {} });
  assert_equals(roundtrip("a"), "5");
}, "char: an import return value is converted with ToString");

for (const [value, description] of [
  ["", "the empty string"],
  ["ab", "two characters"],
  ["\uD800", "a lone surrogate"],
  [Symbol(), "a Symbol"],
]) {
  promise_test(async () => {
    const { roundtrip } = await componentExports("values/char", { echo: () => value, recv() {} });
    assert_traps(() => roundtrip("a"));
  }, `char: an import returning ${description} traps`);
}

promise_test(async () => {
  spied.length = 0;
  exports.send();
  assert_array_equals(spied, ["\0", "a", "퟿", "", "￿", "\u{10000}", "😀", "\u{10FFFF}"]);
  for (const value of spied) {
    assert_equals(typeof value, "string");
    assert_true(value.isWellFormed());
  }
}, "char: guest-produced scalar values lift to one-scalar strings");

promise_test(async () => {
  spied.length = 0;
  assert_traps(() => exports.sendSurrogate());
  assert_array_equals(spied, [], "recv must not be called");
}, "char: lifting a surrogate code point traps");

promise_test(async () => {
  spied.length = 0;
  assert_traps(() => exports.sendOutOfRange());
  assert_array_equals(spied, [], "recv must not be called");
}, "char: lifting a value above U+10FFFF traps");
