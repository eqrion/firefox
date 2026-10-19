// META: global=window,dedicatedworker,jsshell
// META: script=/wasm/component/jsapi/harness/component-helpers.js
// META: script=/wasm/component/jsapi/harness/assertions.js

const funcs = { alpha: () => 1, bravo: () => 2, charlieDelta: () => 4 };

promise_test(async () => {
  const log = [];
  const { run } = await componentExports("imports/lookup", loggingProxy(funcs, log));
  assert_array_equals(log, ["get alpha", "get bravo", "get charlieDelta"]);
  assert_equals(run(), 7);
}, "imports are read in declaration order under their camelCase names, once each");

promise_test(async () => {
  const log = [];
  const imports = {
    get alpha() { log.push("alpha"); return funcs.alpha; },
    get bravo() { log.push("bravo"); return funcs.bravo; },
    get charlieDelta() { log.push("charlieDelta"); return funcs.charlieDelta; },
  };
  await componentExports("imports/lookup", imports);
  assert_array_equals(log, ["alpha", "bravo", "charlieDelta"]);
}, "getters on the imports object are invoked in declaration order");

promise_test(async () => {
  const imports = Object.create(funcs);
  const { run } = await componentExports("imports/lookup", imports);
  assert_equals(run(), 7);
}, "imports are found on the prototype chain");

promise_test(async () => {
  const fn = function () {};
  Object.assign(fn, funcs);
  const { run } = await componentExports("imports/lookup", fn);
  assert_equals(run(), 7);
}, "a function may serve as the imports object");

promise_test(async () => {
  const arr = [];
  Object.assign(arr, funcs);
  const { run } = await componentExports("imports/lookup", arr);
  assert_equals(run(), 7);
}, "an array may serve as the imports object");

promise_test(async t => {
  const error = new Error("from getter");
  const imports = {
    alpha: funcs.alpha,
    get bravo() { throw error; },
    get charlieDelta() { assert_unreached("charlieDelta must not be read"); },
  };
  const component = await compileComponent("imports/lookup");
  assert_throws_exactly(error, () => new WebAssembly.ComponentInstance(component, imports));
  await promise_rejects_exactly(t, error, WebAssembly.instantiate(component, imports));
}, "an exception from a getter propagates unwrapped and stops the walk");

promise_test(async () => {
  const log = [];
  const imports = loggingProxy({ ...funcs, extra: () => 0, "charlie-delta": () => 0 }, log);
  await componentExports("imports/lookup", imports);
  assert_array_equals(log, ["get alpha", "get bravo", "get charlieDelta"]);
}, "properties that are not imports are not read, including the kebab-case spelling");

test_instantiation_error(WebAssembly.LinkError, "imports/lookup", { alpha: funcs.alpha, bravo: funcs.bravo },
                         undefined, "a missing import is a LinkError");
test_instantiation_error(TypeError, "imports/lookup", undefined, undefined,
                         "an undefined imports object for a component with imports is a TypeError");
test_instantiation_error(TypeError, "imports/lookup", 42, undefined,
                         "a non-object imports object is a TypeError");
