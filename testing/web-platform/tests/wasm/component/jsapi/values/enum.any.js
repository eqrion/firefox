// META: global=window,dedicatedworker,jsshell
// META: script=/wasm/component/jsapi/harness/component-helpers.js
// META: script=/wasm/component/jsapi/harness/assertions.js

// enum { red, green, blue, dark-blue, HTML, a1-b2 }

const labels = ["red", "green", "blue", "dark-blue", "HTML", "a1-b2"];

let roundtrip;
let received;
promise_setup(async () => {
  const echo = x => { received = x; return x; };
  ({ roundtrip } = await componentExports("values/enum", { echo }));
});

for (const label of labels) {
  promise_test(async () => {
    const result = roundtrip(label);
    assert_equals(received, label, "value seen by echo");
    assert_equals(result, label);
    assert_equals(typeof result, "string");
  }, `enum: ${JSON.stringify(label)} roundtrips verbatim`);
}

for (const [input, description] of [
  ["purple", "an unknown label"],
  ["Red", "a capitalised label"],
  ["RED", "an upper-cased label"],
  ["html", "a lower-cased acronym label"],
  ["darkBlue", "a camelCase label"],
  ["DarkBlue", "a PascalCase label"],
  ["dark_blue", "a snake_case label"],
  [" red", "a label with leading whitespace"],
  ["", "the empty string"],
  [1, "a number"],
  [null, "null"],
  [undefined, "undefined"],
  [{}, "a plain object"],
  [["red", "green"], "an array of two labels"],
]) {
  promise_test(async () => {
    received = "unchanged";
    assert_throws_js(TypeError, () => roundtrip(input));
    assert_equals(received, "unchanged", "echo must not be called");
  }, `enum: ${description} throws TypeError`);
}

promise_test(async () => {
  received = "unchanged";
  assert_throws_js(TypeError, () => roundtrip(Symbol("red")));
  assert_equals(received, "unchanged", "echo must not be called");
}, "enum: a Symbol throws TypeError from ToString");

promise_test(async () => {
  assert_equals(roundtrip({ toString() { return "blue"; } }), "blue");
  assert_equals(roundtrip(new String("green")), "green");
  assert_equals(roundtrip(["red"]), "red");
  assert_equals(roundtrip({ [Symbol.toPrimitive]() { return "dark-blue"; } }), "dark-blue");
}, "enum: objects are converted with ToString");

promise_test(async () => {
  const { roundtrip } = await componentExports("values/enum", { echo: () => "a1-b2" });
  assert_equals(roundtrip("red"), "a1-b2");
}, "enum: an import may return any label");

promise_test(async () => {
  const { roundtrip } = await componentExports("values/enum", { echo: () => "Red" });
  assert_traps(() => roundtrip("red"));
}, "enum: an import returning an unknown label traps");
