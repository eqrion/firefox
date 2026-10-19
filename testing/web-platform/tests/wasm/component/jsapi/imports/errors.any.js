// META: global=window,dedicatedworker,jsshell
// META: script=/wasm/component/jsapi/harness/component-helpers.js
// META: script=/wasm/component/jsapi/harness/assertions.js

// Which error each import failure produces, and that the first failure stops
// the walk over the imports object.

const cases = [
  [TypeError, "imports/errors-order", undefined, "undefined imports object"],
  [TypeError, "imports/errors-order", null, "null imports object"],
  [TypeError, "imports/errors-order", "s", "string imports object"],
  [WebAssembly.LinkError, "imports/errors-order", { first: 1, second: () => 1 }, "non-callable function import"],
  [WebAssembly.LinkError, "imports/errors-order", { second: () => 1 }, "missing function import"],
  [WebAssembly.LinkError, "imports/errors-type", { R: {} }, "non-callable resource type import"],
  [WebAssembly.LinkError, "imports/errors-type", { R: undefined }, "missing resource type import"],
  [WebAssembly.LinkError, "imports/errors-type", { R: () => ({}) }, "non-constructor for [constructor]"],
  [TypeError, "imports/errors-value", { cfg: -1 }, "value import failing conversion"],
  [TypeError, "imports/errors-value", { cfg: undefined }, "missing value import"],
  [WebAssembly.RuntimeError, "imports/errors-start-trap", { notify: () => {} }, "trapping start function"],
];

for (const [errorCtor, name, imports, description] of cases) {
  test_instantiation_error(errorCtor, name, imports, undefined, `${description} is a ${errorCtor.name}`);
}

promise_test(async () => {
  const { cfg } = await componentExports("imports/errors-value", { cfg: "12" });
  assert_equals(cfg, 12);
}, "a value import is converted with ToComponentValue at instantiation");

promise_test(async t => {
  let notified = 0;
  await promise_rejects_js(t, WebAssembly.RuntimeError,
                           (async () => componentExports("imports/errors-start-trap", { notify: () => notified++ }))());
  assert_equals(notified, 1, "the start function ran until it trapped");
}, "a start function trap is a RuntimeError after side effects are visible");

promise_test(async t => {
  const component = await compileComponent("imports/errors-order");
  const imports = {
    first: 1,
    get second() { assert_unreached("second must not be read"); },
  };
  assert_throws_js(WebAssembly.LinkError, () => new WebAssembly.ComponentInstance(component, imports));
  await promise_rejects_js(t, WebAssembly.LinkError, WebAssembly.instantiate(component, imports));
}, "the first failing import wins and later getters are not read");

promise_test(async t => {
  const component = await compileComponent("imports/errors-order");
  const log = [];
  const imports = {
    get first() { log.push("first"); return () => 1; },
    get second() { log.push("second"); return 2; },
  };
  assert_throws_js(WebAssembly.LinkError, () => new WebAssembly.ComponentInstance(component, imports));
  assert_array_equals(log, ["first", "second"]);
}, "a later import failing still reads every earlier import");

promise_test(async () => {
  const component = await compileComponent("imports/errors-order");
  const imports = { first: () => 1, second: () => 2 };
  const instance = new WebAssembly.ComponentInstance(component, imports);
  assert_equals(instance.exports.run(), 3);
  imports.first = 5;
  assert_equals(instance.exports.run(), 3, "imports are snapshotted at instantiation");
}, "the imports object is read once, at instantiation");
