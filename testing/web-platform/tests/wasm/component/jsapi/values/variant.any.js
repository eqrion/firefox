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
  [{ kind: "none-case" }, variant("none-case"), "a payload-less case"],
  [{ kind: "num", value: 7 }, variant("num", 7), "a u32 payload"],
  [{ kind: "text", value: "hi" }, variant("text", "hi"), "a string payload"],
  [{ kind: "pair", value: [1, 2] }, variant("pair", [1, 2]), "a tuple payload"],
  [{ kind: "undefined" }, variant("undefined"), "a case literally named undefined"],
  [{ kind: "none-case", value: 99 }, variant("none-case"), "a payload-less case ignores value"],
  [{ kind: "num", value: "8" }, variant("num", 8), "the payload is converted to its case type"],
  [{ kind: { toString() { return "num"; } }, value: 1 }, variant("num", 1), "kind goes through ToString"],
  [{ kind: "text" }, variant("text", "undefined"), "a missing string payload becomes \"undefined\""],
  [{ kind: "num" }, variant("num", 0), "a missing u32 payload becomes 0"],
  [{ kind: "num", value: -1 }, variant("num", 0xffffffff), "an out-of-range u32 payload wraps"],
  [{}, variant("undefined"), "a missing kind is the case named undefined, through ToString"],
  [{ kind: undefined }, variant("undefined"), "an undefined kind is the case named undefined, through ToString"],
]) {
  promise_test(async () => {
    const result = roundtrip(input);
    assert_value_equals(received, expected, "value seen by echo");
    assert_value_equals(result, expected);
    assert_equals(Object.getPrototypeOf(result), null, "prototype");
  }, `variant: ${description}`);
}

promise_test(async () => {
  assert_array_equals(Reflect.ownKeys(roundtrip({ kind: "none-case" })), ["kind"]);
  assert_array_equals(Reflect.ownKeys(roundtrip({ kind: "none-case", value: 1 })), ["kind"]);
  assert_array_equals(Reflect.ownKeys(roundtrip({ kind: "num", value: 1 })), ["kind", "value"]);
  assert_false("value" in roundtrip({ kind: "undefined" }));
}, "variant: value is only present for cases with a payload, after kind");

promise_test(async () => {
  const log = [];
  roundtrip(loggingProxy({ kind: "num", value: 1 }, log));
  assert_array_equals(log, ["get kind", "get value"]);
  log.length = 0;
  roundtrip(loggingProxy({ kind: "none-case" }, log));
  assert_array_equals(log, ["get kind"]);
}, "variant: kind is read first and value only for payload cases");

for (const [input, description] of [
  [{ kind: "other" }, "an unknown kind"],
  [{ kind: "Num", value: 1 }, "a PascalCase kind"],
  [{ kind: "noneCase" }, "a camelCase kind"],
  [{ kind: "NONE-CASE" }, "an upper-case kind"],
  [{ kind: "" }, "an empty kind"],
  [{ kind: "Undefined" }, "kind \"Undefined\" rather than \"undefined\""],
  [{ kind: null }, "a null kind"],
  [{ kind: "num", value: 1n }, "an unconvertible payload"],
  [{ kind: "pair" }, "a missing tuple payload"],
  [{ kind: "pair", value: [1] }, "a tuple payload of the wrong arity"],
  [{ kind: "pair", value: [1, 2, 3] }, "a tuple payload with too many elements"],
  [null, "null"],
  [undefined, "undefined"],
  ["num", "a bare string"],
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
  assert_throws_js(TypeError, () => roundtrip({ kind: Symbol("num"), value: 1 }));
  assert_equals(received, "unchanged", "echo must not be called");
}, "variant: a Symbol kind throws TypeError from ToString");

promise_test(async () => {
  class MyError extends Error { name = "MyError"; }
  assert_throws_js(MyError, () => roundtrip({ get kind() { throw new MyError(); } }));
  assert_throws_js(MyError, () => roundtrip({ kind: "num", get value() { throw new MyError(); } }));
}, "variant: a throwing getter propagates its exception");

promise_test(async () => {
  roundtrip({ kind: "pair", value: [1, 2] });
  assert_equals(Object.getPrototypeOf(received), null);
  assert_array_equals(Reflect.ownKeys(received), ["kind", "value"]);
  assert_true(Array.isArray(received.value));
}, "variant: the import receives a null-prototype object");

promise_test(async () => {
  const { roundtrip } = await componentExports("values/variant", { echo: () => ({ kind: "text", value: 5 }) });
  assert_value_equals(roundtrip({ kind: "none-case" }), variant("text", "5"));
}, "variant: an import return value is converted with the same rules");

promise_test(async () => {
  const { roundtrip } = await componentExports("values/variant", { echo: () => ({ kind: "nope" }) });
  assert_traps(() => roundtrip({ kind: "none-case" }));
}, "variant: an import returning an unknown kind traps");
