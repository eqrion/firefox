// META: global=window,dedicatedworker,jsshell
// META: script=/wasm/component/jsapi/harness/component-helpers.js
// META: script=/wasm/component/jsapi/harness/assertions.js

// record { first-name: string, age: u32, nick-name: option<string>,
//          URL: string, maybe: option<option<u32>> }

let roundtrip;
let received;
promise_setup(async () => {
  const echo = x => { received = x; return x; };
  ({ roundtrip } = await componentExports("values/record", { echo }));
});

const full = {
  firstName: "Ada", age: 36, nickName: "ada", url: "https://example.com", maybe: variant("some", 3),
};
const fullExpected = record(full);

promise_test(async () => {
  const result = roundtrip(full);
  assert_value_equals(received, fullExpected, "value seen by echo");
  assert_value_equals(result, fullExpected);
}, "record: every field roundtrips");

promise_test(async () => {
  const result = roundtrip(full);
  assert_equals(Object.getPrototypeOf(result), null, "prototype");
  assert_array_equals(Reflect.ownKeys(result), ["firstName", "age", "nickName", "url", "maybe"]);
  assert_true(Object.isExtensible(result));
}, "record: result is a null-prototype object with camelCase keys in declaration order");

promise_test(async () => {
  const log = [];
  roundtrip(loggingProxy(full, log));
  assert_array_equals(log, ["get firstName", "get age", "get nickName", "get url", "get maybe"]);
}, "record: fields are read with Get in declaration order");

promise_test(async () => {
  const result = roundtrip({ ...full, nickName: null, maybe: null });
  assert_true(Object.hasOwn(result, "nickName"), "nickName is present");
  assert_equals(result.nickName, undefined, "none is undefined");
  assert_true(Object.hasOwn(result, "maybe"), "maybe is present");
  assert_equals(result.maybe, undefined, "none is undefined");
}, "record: none payloads");

promise_test(async () => {
  const result = roundtrip({ ...full, maybe: variant("none") });
  assert_value_equals(result.maybe, variant("none"));
}, "record: option<option<T>> some(none) is undefined outside and the variant form inside");

for (const [fields, expected, description] of [
  [{ age: 1, nickName: null, url: "", maybe: undefined },
   record({ firstName: "undefined", age: 1, nickName: undefined, url: "", maybe: undefined }), "missing firstName"],
  [{ ...full, age: undefined }, record({ ...full, age: 0 }), "age explicitly undefined"],
  [{ firstName: "a", age: 1, nickName: "n", url: "" },
   record({ firstName: "a", age: 1, nickName: "n", url: "", maybe: undefined }), "missing option<option<T>> field"],
  [{ ...full, age: -1 }, record({ ...full, age: 0xffffffff }), "out-of-range age"],
]) {
  promise_test(async () => {
    const result = roundtrip(fields);
    assert_value_equals(received, expected, "value seen by echo");
    assert_value_equals(result, expected);
  }, `record: ${description} is converted like undefined`);
}

for (const [fields, description] of [
  [{ ...full, firstName: Symbol() }, "Symbol for a string field"],
  [{ ...full, age: 1n }, "BigInt for a u32 field"],
  [{ ...full, maybe: 1 }, "a bare payload for an option<option<T>> field"],
]) {
  // A failure after first-name is stored has entered the guest to allocate the
  // string, which locks down the instance, so each case uses a fresh one.
  promise_test(async () => {
    received = "unchanged";
    const { roundtrip } = await componentExports("values/record", {
      echo: x => { received = x; return x; },
    });
    assert_throws_js(TypeError, () => roundtrip(fields));
    assert_equals(received, "unchanged", "echo must not be called");
  }, `record: ${description} throws TypeError`);
}

promise_test(async () => {
  const result = roundtrip({ ...full, age: null, firstName: null });
  assert_equals(result.age, 0, "null age is 0");
  assert_equals(result.firstName, "null", "null firstName is \"null\"");
}, "record: null is a value, not a missing field");

promise_test(async () => {
  const result = roundtrip({ firstName: "a", age: 1, url: "", maybe: undefined });
  assert_equals(result.nickName, undefined);
  const result2 = roundtrip({ ...full, nickName: undefined });
  assert_equals(result2.nickName, undefined);
}, "record: a missing or undefined option field is none");

promise_test(async () => {
  const result = roundtrip(Object.create(full));
  assert_value_equals(result, fullExpected);
}, "record: inherited properties are honoured");

promise_test(async () => {
  const result = roundtrip({ ...full, extra: 1, [Symbol.iterator]: null });
  assert_value_equals(result, fullExpected);
}, "record: extra properties are ignored");

promise_test(async () => {
  const result = roundtrip(new Proxy({}, {
    get(target, key) {
      return full[key];
    },
  }));
  assert_value_equals(result, fullExpected);
}, "record: a Proxy is read through its get trap");

for (const value of [null, undefined, "firstName", 1, true, Symbol(), 1n]) {
  promise_test(async () => {
    received = "unchanged";
    assert_throws_js(TypeError, () => roundtrip(value));
    assert_equals(received, "unchanged", "echo must not be called");
  }, `record: non-object ${format_value(value)} throws TypeError`);
}

promise_test(async () => {
  class MyError extends Error { name = "MyError"; }
  const input = { ...full, get age() { throw new MyError(); } };
  const { roundtrip } = await componentExports("values/record", { echo: x => x });
  assert_throws_js(MyError, () => roundtrip(input));
}, "record: a throwing getter propagates its exception");

promise_test(async () => {
  const { roundtrip } = await componentExports("values/record", { echo: () => ({ ...full, age: 1n }) });
  assert_traps(() => roundtrip(full));
}, "record: an import returning a record with an unconvertible field traps");

promise_test(async () => {
  const { roundtrip } = await componentExports("values/record", { echo: () => ({ firstName: 1, age: "2", url: "u", maybe: variant("none") }) });
  assert_value_equals(roundtrip(full), record({ firstName: "1", age: 2, nickName: undefined, url: "u", maybe: variant("none") }));
}, "record: an import return value is converted field by field");

promise_test(async () => {
  roundtrip(full);
  assert_equals(Object.getPrototypeOf(received), null);
  assert_array_equals(Reflect.ownKeys(received), ["firstName", "age", "nickName", "url", "maybe"]);
}, "record: the import receives a null-prototype object");
