// META: global=window,dedicatedworker,jsshell
// META: script=/wasm/component/jsapi/harness/component-helpers.js
// META: script=/wasm/component/jsapi/harness/assertions.js

class R {}
const good = { plain: x => x, noresult: () => {}, R };

promise_test(async () => {
  const { callPlain, callNoresult, make } = await componentExports("imports/functions", good);
  assert_equals(callPlain(4), 4);
  assert_equals(callNoresult(4), undefined);
  assert_true(make() instanceof R);
}, "callable imports link and are invoked");

for (const [value, description] of [
  [undefined, "undefined"], [null, "null"], [{}, "plain object"], [1, "number"],
  ["f", "string"], [new R(), "class instance"], [Symbol(), "symbol"],
]) {
  test_instantiation_error(WebAssembly.LinkError, "imports/functions", { ...good, plain: value }, undefined,
                           `a non-callable function import (${description}) is a LinkError`);
}

for (const [value, description] of [
  [x => x, "arrow function"],
  [function (x) { return x; }, "function expression"],
  [(x => x).bind(null), "bound function"],
  [new Proxy(x => x, {}), "callable proxy"],
  [Math.abs, "built-in function"],
]) {
  promise_test(async () => {
    const { callPlain } = await componentExports("imports/functions", { ...good, plain: value });
    assert_equals(callPlain(1), 1);
  }, `${description} satisfies a plain function import`);
}

promise_test(async () => {
  const { callPlain } = await componentExports("imports/functions", { ...good, plain: class {} });
  assert_traps(() => callPlain(1));
}, "a class used as a plain function import traps when called");

promise_test(async () => {
  const { callPlain } = await componentExports("imports/functions", { ...good, plain: async x => x });
  assert_equals(callPlain(1), 0, "a Promise converts to u32 like any other object");
}, "an async function satisfies the import and its Promise result converts like an object");

test_instantiation_error(WebAssembly.LinkError, "imports/functions", { ...good, R: () => ({}) }, undefined,
                         "a non-constructor for a [constructor] import is a LinkError");
test_instantiation_error(WebAssembly.LinkError, "imports/functions", { ...good, R: Math.abs }, undefined,
                         "a built-in non-constructor for a [constructor] import is a LinkError");

promise_test(async () => {
  const { make } = await componentExports("imports/functions", { ...good, R: R.bind(null) });
  assert_true(make() instanceof R);
}, "a bound constructor satisfies a [constructor] import");

promise_test(async () => {
  let receiver = "unset";
  const { callPlain } = await componentExports("imports/functions", {
    ...good,
    plain(x) { "use strict"; receiver = this; return x; },
  });
  callPlain(1);
  assert_equals(receiver, undefined);
}, "a plain function import is called with this === undefined");

promise_test(async () => {
  let newTarget = "unset";
  const { callPlain } = await componentExports("imports/functions", {
    ...good,
    plain(x) { newTarget = new.target; return x; },
  });
  callPlain(1);
  assert_equals(newTarget, undefined);
}, "a plain function import is called, not constructed");

promise_test(async () => {
  const received = [];
  const { callPlain } = await componentExports("imports/functions", {
    ...good,
    plain(...args) { received.push(args); return 0; },
  });
  callPlain(9);
  assert_equals(received.length, 1);
  assert_array_equals(received[0], [9]);
  assert_equals(typeof received[0][0], "number");
}, "arguments arrive converted with ToJSValue");

promise_test(async () => {
  const { callPlain } = await componentExports("imports/functions", { ...good, plain: () => "12" });
  assert_equals(callPlain(0), 12);
}, "the return value is converted with ToComponentValue");

promise_test(async () => {
  for (const value of [Symbol(), {}, undefined, () => {}, 1n]) {
    const { callNoresult } = await componentExports("imports/functions", { ...good, noresult: () => value });
    assert_equals(callNoresult(1), undefined, `return of ${format_value(value)} is ignored`);
  }
}, "a return value is ignored when the import has no result type");

promise_test(async () => {
  const log = [];
  const imports = loggingProxy(good, log);
  await componentExports("imports/functions", imports);
  assert_array_equals(log, ["get plain", "get noresult", "get R"]);
}, "a [constructor] import is not read from the imports object");
