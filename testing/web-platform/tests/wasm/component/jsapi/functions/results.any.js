// META: global=window,dedicatedworker,jsshell
// META: script=/wasm/component/jsapi/harness/component-helpers.js
// META: script=/wasm/component/jsapi/harness/assertions.js

function makeImports(overrides = {}) {
  return {
    fallible: x => x,
    fallibleU32: () => 1,
    fallibleNone: () => {},
    fallibleNoOk: () => {},
    nested: () => 5,
    plain: () => 3,
    ...overrides,
  };
}

// Imports are looked up under their camelCase JS names.
function camel(imports) {
  const out = {};
  for (const [key, value] of Object.entries(imports)) {
    out[key.replace(/-([a-z0-9])/g, (_, c) => c.toUpperCase())] = value;
  }
  return out;
}

async function load(overrides) {
  return componentExports("functions/results", camel(makeImports(overrides)));
}

promise_test(async () => {
  const { succeed, fail } = await load();
  assert_equals(succeed(), 7);
  const error = assert_component_error(() => fail(), "boom", "fail()");
  assert_equals(error.constructor, WebAssembly.ComponentError);
}, "an exported result is unwrapped: ok returns, error throws ComponentError");

promise_test(async () => {
  const { failNone, succeedNone } = await load();
  assert_component_error(() => failNone(), undefined, "fail-none()");
  assert_equals(succeedNone(), undefined);
}, "payload-less ok returns undefined and payload-less error has undefined data");

promise_test(async () => {
  const { resultParam } = await load();
  assert_equals(resultParam(variant("ok", 3)), 0);
  assert_equals(resultParam(variant("error", "x")), 1);
  assert_equals(resultParam({ kind: "ok", value: 1 }), 0, "any object with kind works");
  assert_throws_js(TypeError, () => resultParam({ kind: "err", value: "x" }));
  assert_throws_js(TypeError, () => resultParam({ kind: "Ok", value: 1 }));
  assert_throws_js(TypeError, () => resultParam(3));
}, "a result outside return position is a variant with kind ok or error");

promise_test(async () => {
  const { callFallible } = await load({ fallible: x => x + 1 });
  assert_equals(callFallible(1), 2);
}, "an import returning normally produces ok");

promise_test(async () => {
  const { callFallible } = await load({ fallible: () => { throw "bad key"; } });
  assert_component_error(() => callFallible(1), "bad key", "callFallible()");
}, "an import throwing produces error with the thrown value converted");

promise_test(async () => {
  const { callFallible } = await load({ fallible: () => { throw new Error("oops"); } });
  assert_component_error(() => callFallible(1), "Error: oops", "callFallible()");
}, "a thrown Error object is converted with ToString for a string error type");

promise_test(async () => {
  const { callFallibleU32 } = await load({ fallibleU32: () => { throw 42; } });
  assert_component_error(() => callFallibleU32(), 42, "callFallibleU32()");
}, "a thrown value is converted to the error type");

promise_test(async () => {
  const { callFallibleU32 } = await load({ fallibleU32: () => { throw 1n; } });
  assert_traps(() => callFallibleU32());
}, "a thrown value that cannot convert to the error type traps");

promise_test(async () => {
  const { callFallibleNone } = await load({ fallibleNone: () => "ignored" });
  assert_equals(callFallibleNone(), undefined);
}, "an import with no ok payload ignores its return value");

promise_test(async () => {
  const thrown = { some: "object" };
  const { callFallibleNone } = await load({ fallibleNone: () => { throw thrown; } });
  const error = assert_component_error(() => callFallibleNone(), undefined, "callFallibleNone()");
  assert_not_equals(error, thrown, "the thrown value is discarded");
}, "an import throwing with no error payload produces a payload-less error");

promise_test(async () => {
  const { callFallibleNoOk } = await load({ fallibleNoOk: () => 7 });
  assert_equals(callFallibleNoOk(), undefined);
  const { callFallibleNoOk: failing } = await load({ fallibleNoOk: () => { throw "no"; } });
  assert_component_error(() => failing(), "no", "callFallibleNoOk()");
}, "result<_, E>: return value is ignored, throw becomes error with E");

promise_test(async () => {
  const { callFallibleRecord } = await componentExports("functions/results-record", {
    fallibleRecord() { throw { code: 4, message: "not found" }; },
  });
  assert_component_error(() => callFallibleRecord(),
                         record({ code: 4, message: "not found" }), "callFallibleRecord()");
}, "a record error payload is converted field by field");

promise_test(async () => {
  const { callFallibleRecord } = await componentExports("functions/results-record", {
    fallibleRecord() { throw { code: 4 }; },
  });
  assert_component_error(() => callFallibleRecord(),
                         record({ code: 4, message: "undefined" }), "callFallibleRecord()");
}, "a thrown object missing an error field converts it from undefined");

promise_test(async () => {
  const { callFallibleRecord } = await componentExports("functions/results-record", {
    fallibleRecord() { throw { code: 1n, message: "m" }; },
  });
  assert_traps(() => callFallibleRecord());
}, "a thrown object with an unconvertible error field traps");

promise_test(async () => {
  const { callNested } = await load({ nested: () => variant("ok", 9) });
  assert_value_equals(callNested(), variant("ok", 9));
  const { callNested: erroring } = await load({ nested: () => { throw "outer"; } });
  assert_component_error(() => erroring(), "outer", "callNested()");
}, "only the outermost result is unwrapped; the inner one keeps its variant shape");

promise_test(async () => {
  const { callNested } = await load({ nested: () => ({ kind: "error", value: "inner" }) });
  assert_value_equals(callNested(), variant("error", "inner"));
}, "an import returns a nested result as a variant object");

promise_test(async () => {
  const thrown = new Error("escaped");
  const { callPlain } = await load({ plain: () => { throw thrown; } });
  let caught;
  try {
    callPlain();
  } catch (e) {
    caught = e;
  }
  assert_true(caught instanceof WebAssembly.RuntimeError, "a RuntimeError is thrown");
  assert_not_equals(caught, thrown, "the original exception is not observable");
}, "an import throwing where the type is not a result traps");

promise_test(async () => {
  const { callPlain } = await load({ plain: () => Symbol() });
  assert_traps(() => callPlain());
}, "an import return value that cannot be converted traps");

promise_test(async () => {
  const { callFallible } = await load({ fallible: () => Symbol() });
  assert_traps(() => callFallible(1));
}, "an import ok value that cannot be converted traps even for a result type");

promise_test(async () => {
  const { fail } = await load();
  let first;
  try {
    fail();
  } catch (e) {
    first = e;
  }
  assert_true(first instanceof WebAssembly.ComponentError);
  assert_equals(first.data, "boom");
  assert_equals(typeof first.message, "string");
  assert_false(Object.hasOwn(first, "data"), "data is an accessor on the prototype");
}, "ComponentError thrown by an export has data and a message");
