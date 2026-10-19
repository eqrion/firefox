// META: global=window,dedicatedworker,jsshell
// META: script=/wasm/component/jsapi/harness/component-helpers.js
// META: script=/wasm/component/jsapi/harness/assertions.js

let roundtrip;
let received;
promise_setup(async () => {
  const echo = x => { received = x; return x; };
  ({ roundtrip } = await componentExports("values/string", { echo }));
});

for (const [value, description] of [
  ["", "empty"],
  ["hello", "ASCII"],
  ["héllo wörld", "Latin-1"],
  ["こんにちは", "BMP multi-byte"],
  ["😀 \u{1F600}", "astral"],
  ["a\0b", "embedded NUL"],
  ["퟿�", "surrogate range boundaries"],
  ["x".repeat(1 << 20), "1 MiB"],
]) {
  promise_test(async () => {
    const result = roundtrip(value);
    assert_equals(received, value, "value seen by echo");
    assert_equals(result, value);
    assert_true(result.isWellFormed());
  }, `string: ${description} roundtrips exactly`);
}

for (const [input, expected, description] of [
  [42, "42", "number"],
  [null, "null", "null"],
  [undefined, "undefined", "undefined"],
  [true, "true", "boolean"],
  [{ toString() { return "obj"; } }, "obj", "object with toString"],
  [["a", "b"], "a,b", "array"],
  [1n, "1", "BigInt"],
]) {
  promise_test(async () => {
    assert_equals(roundtrip(input), expected);
    assert_equals(received, expected, "value seen by echo");
  }, `string: ${description} is converted with ToString`);
}

for (const [input, expected, description] of [
  ["\uD800", "�", "lone lead surrogate"],
  ["\uDC00", "�", "lone trail surrogate"],
  ["a\uD800b", "a�b", "lone lead surrogate in the middle"],
  ["\uDC00\uD800", "��", "reversed pair"],
  ["😀\uD83D", "😀�", "valid pair followed by lone lead"],
]) {
  promise_test(async () => {
    assert_equals(roundtrip(input), expected);
    assert_equals(received, expected, "value seen by echo");
  }, `string: ${description} is replaced with U+FFFD`);
}

promise_test(async () => {
  received = "unchanged";
  assert_throws_js(TypeError, () => roundtrip(Symbol()));
  assert_equals(received, "unchanged", "echo must not be called");
}, "string: Symbol throws TypeError");

promise_test(async () => {
  const { roundtrip } = await componentExports("values/string", { echo: () => Symbol() });
  assert_traps(() => roundtrip("x"));
}, "string: an import returning a Symbol traps");

promise_test(async () => {
  const { roundtrip } = await componentExports("values/string", { echo: () => 12 });
  assert_equals(roundtrip("x"), "12");
}, "string: an import return value is converted with ToString");

promise_test(async () => {
  const { roundtrip } = await componentExports("values/string", { echo: () => "\uD800" });
  assert_equals(roundtrip("x"), "�");
}, "string: lone surrogates in an import return value are replaced");
