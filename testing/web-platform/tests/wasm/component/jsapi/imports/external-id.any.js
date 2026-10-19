// META: global=window,dedicatedworker,jsshell
// META: script=/wasm/component/jsapi/harness/component-helpers.js
// META: script=/wasm/component/jsapi/harness/assertions.js

// An import with an `external-id` is read from the imports object under the
// external-id string instead of its JS name.

function makeImports(log = []) {
  const target = {
    "https://example.com/log": message => log.push(message),
    same: () => 5,
    "resource id": class {},
    "value id": 7,
    "instance id": { f: () => 10 },
  };
  return target;
}

promise_test(async () => {
  const messages = [];
  const { run, sum, cfg } = await componentExports("imports/external-id", makeImports(messages));
  run();
  assert_array_equals(messages, ["hello"]);
  assert_equals(sum(), 20);
  assert_equals(cfg, 7);
}, "imports are resolved through their external-id for every sort");

promise_test(async () => {
  const log = [];
  await componentExports("imports/external-id", loggingProxy(makeImports(), log));
  assert_array_equals(log, [
    "get https://example.com/log",
    "get same",
    "get same",
    "get resource id",
    "get value id",
    "get instance id",
  ]);
}, "the lookup keys are the external-ids, in declaration order, once per import");

promise_test(async () => {
  const log = [];
  const imports = loggingProxy({ ...makeImports(), log: () => {}, dup1: () => 1, dup2: () => 1,
                                 R: class {}, cfg: 1, iface: {} }, log);
  await componentExports("imports/external-id", imports);
  assert_false(log.some(entry => /get (log|dup1|dup2|R|cfg|iface)$/.test(entry)),
               `JS names must not be read: ${log}`);
}, "the JS name is not consulted when an external-id is present");

promise_test(async () => {
  let calls = 0;
  const imports = { ...makeImports(), same: () => ++calls };
  const { sum } = await componentExports("imports/external-id", imports);
  assert_equals(sum(), 13, "both dup imports call the same function");
  assert_equals(calls, 2);
}, "two imports sharing an external-id resolve to the same value");

promise_test(async () => {
  const { encode } = await componentExports("imports/external-id-builtin", {}, { builtins: ["js/global"] });
  assert_equals(encode("hi"), btoa("hi"));
}, "an external-id naming a builtin specifier resolves to the builtin when enabled");

promise_test(async () => {
  const log = [];
  const imports = loggingProxy({ "wasm:js/global": { btoa: () => "never" }, g: { btoa: () => "never" } }, log);
  const { encode } = await componentExports("imports/external-id-builtin", imports, { builtins: ["js/global"] });
  assert_equals(encode("hi"), btoa("hi"));
  assert_array_equals(log, [], "the imports object is not consulted for an enabled builtin");
}, "an enabled builtin external-id bypasses the imports object");

promise_test(async () => {
  const { encode } = await componentExports("imports/external-id-builtin",
                                            { "wasm:js/global": { btoa: s => `custom:${s}` } });
  assert_equals(encode("hi"), "custom:hi");
}, "a builtin external-id falls back to the imports object when not enabled");

test_instantiation_error(WebAssembly.LinkError, "imports/external-id",
                         { ...makeImports(), "https://example.com/log": undefined }, undefined,
                         "a missing external-id property is a LinkError");
