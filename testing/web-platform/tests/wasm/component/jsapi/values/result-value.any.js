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
  [{ kind: "ok", value: 1 }, variant("ok", 1), "ok"],
  [{ kind: "error", value: "bad" }, variant("error", "bad"), "error"],
  [{ kind: "ok", value: "2" }, variant("ok", 2), "ok payload is converted"],
  [{ kind: "error", value: 3 }, variant("error", "3"), "error payload is converted"],
  [{ kind: "error" }, variant("error", "undefined"), "a missing string payload becomes \"undefined\""],
  [{ kind: "ok" }, variant("ok", 0), "a missing u32 payload becomes 0"],
]) {
  promise_test(async () => {
    const result = exports.roundtripParam(input);
    assert_value_equals(received, expected, "value seen by echo");
    assert_value_equals(result, expected);
  }, `result as a parameter: ${description}`);
}

for (const [input, description] of [
  [{ kind: "err", value: "x" }, "kind err"],
  [{ kind: "Ok", value: 1 }, "PascalCase kind"],
  [{ kind: "ok", value: 1n }, "ok with a BigInt payload"],
  [{ kind: "error", value: Symbol() }, "error with a Symbol payload"],
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
  assert_equals(roundtripParam({ kind: "ok", value: 1 }), undefined);
}, "result inside option: an import returning null yields none rather than a thrown error");

promise_test(async () => {
  const { roundtripParam } = await componentExports("values/result-value", {
    echoParam: () => { throw new Error("boom"); }, echoList: x => x, echoRecord: x => x,
  });
  assert_traps(() => roundtripParam({ kind: "ok", value: 1 }));
}, "result inside option: a throwing import is not captured as an error result");

promise_test(async () => {
  const input = [{ kind: "ok", value: 1 }, { kind: "error", value: "x" }, { kind: "ok", value: 0 }];
  const expected = [variant("ok", 1), variant("error", "x"), variant("ok", 0)];
  const result = exports.roundtripList(input);
  assert_value_equals(received, expected, "value seen by echo");
  assert_value_equals(result, expected);
  assert_value_equals(exports.roundtripList([]), []);
}, "result as a list element");

promise_test(async () => {
  const input = { res: { kind: "ok" }, ok: { kind: "ok", value: 5 }, plain: { kind: "error" } };
  const expected = record({ res: variant("ok"), ok: variant("ok", 5), plain: variant("error") });
  const result = exports.roundtripRecord(input);
  assert_value_equals(received, expected, "value seen by echo");
  assert_value_equals(result, expected);
  assert_array_equals(Reflect.ownKeys(result.res), ["kind"]);
  assert_array_equals(Reflect.ownKeys(result.plain), ["kind"]);
}, "result as a record field: payload-less ok and error have no value property");

promise_test(async () => {
  const input = { res: { kind: "error", value: "e" }, ok: { kind: "error" }, plain: { kind: "ok", value: 3 } };
  const expected = record({ res: variant("error", "e"), ok: variant("error"), plain: variant("ok") });
  assert_value_equals(exports.roundtripRecord(input), expected);
}, "result as a record field: a payload is ignored on a payload-less case");

promise_test(async () => {
  assert_throws_js(TypeError, () => exports.roundtripRecord({ res: { kind: "ok" }, ok: { kind: "ok", value: 1n }, plain: { kind: "ok" } }),
                   "result<u32> ok with a BigInt payload");
  assert_throws_js(TypeError, () => exports.roundtripRecord({ res: { kind: "error", value: Symbol() }, ok: { kind: "ok", value: 1 }, plain: { kind: "ok" } }),
                   "result<_, string> error with a Symbol payload");
}, "result as a record field: an unconvertible payload for a payload case throws TypeError");
