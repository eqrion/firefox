// META: global=window,dedicatedworker,jsshell
// META: script=/wasm/component/jsapi/harness/component-helpers.js
// META: script=/wasm/component/jsapi/harness/assertions.js

// result<T, E> outside return position keeps the variant shape:
//   roundtrip-param(x: result<u32, string>) -> option<result<u32, string>>
//   roundtrip-list(x: list<result<u32, string>>) -> list<result<u32, string>>
//   roundtrip-record(x: outcome) -> outcome
//     where outcome = record { res: result<_, string>, ok: result<u32>, plain: result }

let exports;
let received;
promise_setup(async () => {
  const echo = x => { received = x; return x; };
  exports = await componentExports("values/result-value", {
    echoParam: echo, echoList: echo, echoRecord: echo,
  });
});

for (const [input, expected, description] of [
  [{ kind: "Ok", value: 1 }, variant("Ok", 1), "ok"],
  [{ kind: "Error", value: "bad" }, variant("Error", "bad"), "error"],
  [{ kind: "Ok", value: "2" }, variant("Ok", 2), "ok payload is converted"],
  [{ kind: "Error", value: 3 }, variant("Error", "3"), "error payload is converted"],
  [{ kind: "Error" }, variant("Error", "undefined"), "a missing string payload becomes \"undefined\""],
]) {
  promise_test(async () => {
    const result = exports.roundtripParam(input);
    assert_value_equals(received, expected, "value seen by echo");
    assert_value_equals(result, expected);
  }, `result as a parameter: ${description}`);
}

for (const [input, description] of [
  [{ kind: "Err", value: "x" }, "kind Err"],
  [{ kind: "ok", value: 1 }, "lower-case kind"],
  [{ kind: "Ok" }, "ok without a u32 payload"],
  [{ kind: "Error", value: Symbol() }, "error with a Symbol payload"],
  [null, "null"],
  [1, "a bare number"],
]) {
  promise_test(async () => {
    received = "unchanged";
    assert_throws_js(TypeError, () => exports.roundtripParam(input));
    assert_equals(received, "unchanged", "echo must not be called");
  }, `result as a parameter: ${description} throws TypeError`);
}

promise_test(async () => {
  const { roundtripParam } = await componentExports("values/result-value", {
    echoParam: () => null, echoList: x => x, echoRecord: x => x,
  });
  assert_equals(roundtripParam({ kind: "Ok", value: 1 }), null);
}, "result inside option: an import returning null yields none rather than a thrown error");

promise_test(async () => {
  const { roundtripParam } = await componentExports("values/result-value", {
    echoParam: () => { throw new Error("boom"); }, echoList: x => x, echoRecord: x => x,
  });
  assert_traps(() => roundtripParam({ kind: "Ok", value: 1 }));
}, "result inside option: a throwing import is not captured as an error result");

promise_test(async () => {
  const input = [{ kind: "Ok", value: 1 }, { kind: "Error", value: "x" }, { kind: "Ok", value: 0 }];
  const expected = [variant("Ok", 1), variant("Error", "x"), variant("Ok", 0)];
  const result = exports.roundtripList(input);
  assert_value_equals(received, expected, "value seen by echo");
  assert_value_equals(result, expected);
  assert_value_equals(exports.roundtripList([]), []);
}, "result as a list element");

promise_test(async () => {
  const input = { res: { kind: "Ok" }, ok: { kind: "Ok", value: 5 }, plain: { kind: "Error" } };
  const expected = record({ res: variant("Ok"), ok: variant("Ok", 5), plain: variant("Error") });
  const result = exports.roundtripRecord(input);
  assert_value_equals(received, expected, "value seen by echo");
  assert_value_equals(result, expected);
  assert_array_equals(Reflect.ownKeys(result.res), ["kind"]);
  assert_array_equals(Reflect.ownKeys(result.plain), ["kind"]);
}, "result as a record field: payload-less ok and error have no value property");

promise_test(async () => {
  const input = { res: { kind: "Error", value: "e" }, ok: { kind: "Error" }, plain: { kind: "Ok", value: 3 } };
  const expected = record({ res: variant("Error", "e"), ok: variant("Error"), plain: variant("Ok") });
  assert_value_equals(exports.roundtripRecord(input), expected);
}, "result as a record field: a payload is ignored on a payload-less case");

promise_test(async () => {
  assert_throws_js(TypeError, () => exports.roundtripRecord({ res: { kind: "Ok" }, ok: { kind: "Ok" }, plain: { kind: "Ok" } }),
                   "result<u32> ok without payload");
  assert_throws_js(TypeError, () => exports.roundtripRecord({ res: { kind: "Error", value: Symbol() }, ok: { kind: "Ok", value: 1 }, plain: { kind: "Ok" } }),
                   "result<_, string> error with a Symbol payload");
}, "result as a record field: a missing or unconvertible payload for a payload case throws TypeError");
