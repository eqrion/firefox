// META: global=window,dedicatedworker,jsshell
// META: script=/wasm/component/jsapi/harness/component-helpers.js
// META: script=/wasm/component/jsapi/harness/assertions.js

// variant { none-case, num(u32), text(string), pair(tuple<u32, u32>), undefined }

let roundtrip;
let received;
promise_setup(async () => {
  const echo = x => { received = x; return x; };
  ({ roundtrip } = await componentExports("values/variant", { echo }));
});

for (const [input, expected, description] of [
  [{ kind: "NoneCase" }, variant("NoneCase"), "a payload-less case"],
  [{ kind: "Num", value: 7 }, variant("Num", 7), "a u32 payload"],
  [{ kind: "Text", value: "hi" }, variant("Text", "hi"), "a string payload"],
  [{ kind: "Pair", value: [1, 2] }, variant("Pair", [1, 2]), "a tuple payload"],
  [{ kind: "Undefined" }, variant("Undefined"), "a case literally named undefined"],
  [{ kind: "NoneCase", value: 99 }, variant("NoneCase"), "a payload-less case ignores value"],
  [{ kind: "Num", value: "8" }, variant("Num", 8), "the payload is converted to its case type"],
  [{ kind: { toString() { return "Num"; } }, value: 1 }, variant("Num", 1), "kind goes through ToString"],
  [{ kind: "Text" }, variant("Text", "undefined"), "a missing string payload becomes \"undefined\""],
]) {
  promise_test(async () => {
    const result = roundtrip(input);
    assert_value_equals(received, expected, "value seen by echo");
    assert_value_equals(result, expected);
    assert_equals(Object.getPrototypeOf(result), null, "prototype");
  }, `variant: ${description}`);
}

promise_test(async () => {
  assert_array_equals(Reflect.ownKeys(roundtrip({ kind: "NoneCase" })), ["kind"]);
  assert_array_equals(Reflect.ownKeys(roundtrip({ kind: "NoneCase", value: 1 })), ["kind"]);
  assert_array_equals(Reflect.ownKeys(roundtrip({ kind: "Num", value: 1 })), ["kind", "value"]);
  assert_false("value" in roundtrip({ kind: "Undefined" }));
}, "variant: value is only present for cases with a payload, after kind");

promise_test(async () => {
  const log = [];
  roundtrip(loggingProxy({ kind: "Num", value: 1 }, log));
  assert_array_equals(log, ["get kind", "get value"]);
  log.length = 0;
  roundtrip(loggingProxy({ kind: "NoneCase" }, log));
  assert_array_equals(log, ["get kind"]);
}, "variant: kind is read first and value only for payload cases");

for (const [input, description] of [
  [{ kind: "Other" }, "an unknown kind"],
  [{ kind: "num", value: 1 }, "a lower-case kind"],
  [{ kind: "none-case" }, "a kebab-case kind"],
  [{ kind: "NONECASE" }, "an upper-case kind"],
  [{ kind: "" }, "an empty kind"],
  [{}, "a missing kind, even though a case is named undefined"],
  [{ kind: undefined }, "an undefined kind, even though a case is named undefined"],
  [{ kind: "undefined" }, "kind \"undefined\" rather than \"Undefined\""],
  [{ kind: null }, "a null kind"],
  [{ kind: "Num" }, "a missing u32 payload"],
  [{ kind: "Num", value: -1 }, "an out-of-range payload"],
  [{ kind: "Pair" }, "a missing tuple payload"],
  [{ kind: "Pair", value: [1] }, "a tuple payload of the wrong arity"],
  [{ kind: "Pair", value: [1, 2, 3] }, "a tuple payload with too many elements"],
  [null, "null"],
  [undefined, "undefined"],
  ["Num", "a bare string"],
  [1, "a bare number"],
]) {
  promise_test(async () => {
    received = "unchanged";
    assert_throws_js(TypeError, () => roundtrip(input));
    assert_equals(received, "unchanged", "echo must not be called");
  }, `variant: ${description} throws TypeError`);
}

promise_test(async () => {
  received = "unchanged";
  assert_throws_js(TypeError, () => roundtrip({ kind: Symbol("Num"), value: 1 }));
  assert_equals(received, "unchanged", "echo must not be called");
}, "variant: a Symbol kind throws TypeError from ToString");

promise_test(async () => {
  class MyError extends Error { name = "MyError"; }
  assert_throws_js(MyError, () => roundtrip({ get kind() { throw new MyError(); } }));
  assert_throws_js(MyError, () => roundtrip({ kind: "Num", get value() { throw new MyError(); } }));
}, "variant: a throwing getter propagates its exception");

promise_test(async () => {
  roundtrip({ kind: "Pair", value: [1, 2] });
  assert_equals(Object.getPrototypeOf(received), null);
  assert_array_equals(Reflect.ownKeys(received), ["kind", "value"]);
  assert_true(Array.isArray(received.value));
}, "variant: the import receives a null-prototype object");

promise_test(async () => {
  const { roundtrip } = await componentExports("values/variant", { echo: () => ({ kind: "Text", value: 5 }) });
  assert_value_equals(roundtrip({ kind: "NoneCase" }), variant("Text", "5"));
}, "variant: an import return value is converted with the same rules");

promise_test(async () => {
  const { roundtrip } = await componentExports("values/variant", { echo: () => ({ kind: "Nope" }) });
  assert_traps(() => roundtrip({ kind: "NoneCase" }));
}, "variant: an import returning an unknown kind traps");
