// META: global=window,dedicatedworker,jsshell
// META: script=/wasm/component/jsapi/harness/component-helpers.js
// META: script=/wasm/component/jsapi/harness/assertions.js

// The canonical ABI string encoding is a guest-side detail. JS must see the
// same strings whether the component uses utf8, utf16 or latin1+utf16.

const inputs = [
  ["", ""],
  ["hello", "hello"],
  ["héllo wörld ÿ", "héllo wörld ÿ"],
  ["Ā", "Ā"],
  ["こんにちは", "こんにちは"],
  ["😀 \u{1F600} \u{10FFFF}", "😀 \u{1F600} \u{10FFFF}"],
  ["a\0b", "a\0b"],
  ["\uD800", "�"],
  ["a\uDC00b", "a�b"],
  ["😀\uD83D", "😀�"],
  ["é".repeat(70000), "é".repeat(70000)],
  ["😀".repeat(70000), "😀".repeat(70000)],
];

const fixtures = {
  utf8: "values/string",
  utf16: "values/string-utf16",
  "latin1+utf16": "values/string-latin1-utf16",
};

const exports = {};
const received = {};
promise_setup(async () => {
  for (const [encoding, name] of Object.entries(fixtures)) {
    exports[encoding] = await componentExports(name, {
      echo: x => { received[encoding] = x; return x; },
    });
  }
});

for (const [encoding] of Object.entries(fixtures)) {
  for (const [input, expected] of inputs) {
    // JSON.stringify escapes lone surrogates, which test names cannot contain.
    const label = input.length > 20
      ? `${JSON.stringify(input.slice(0, 8))}... (${input.length} code units)`
      : JSON.stringify(input);
    promise_test(async () => {
      const result = exports[encoding].roundtrip(input);
      assert_equals(received[encoding], expected, "value seen by echo");
      assert_equals(result, expected);
      assert_true(result.isWellFormed());
    }, `string-encoding=${encoding}: ${label}`);
  }
}

promise_test(async () => {
  for (const [input] of inputs) {
    const results = Object.values(exports).map(e => e.roundtrip(input));
    for (const result of results) {
      assert_equals(result, results[0], `all encodings agree on ${format_value(input.slice(0, 8))}`);
    }
  }
}, "every string encoding produces identical JS strings");

promise_test(async () => {
  const e = await componentExports(fixtures["latin1+utf16"], { echo: () => 42 });
  assert_equals(e.roundtrip("x"), "42");
  const e2 = await componentExports(fixtures.utf16, { echo: () => Symbol() });
  assert_traps(() => e2.roundtrip("x"));
}, "string-encoding: import return conversion does not depend on the encoding");
