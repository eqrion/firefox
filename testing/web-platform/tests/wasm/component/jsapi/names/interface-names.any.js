// META: global=window,dedicatedworker,jsshell
// META: script=/wasm/component/jsapi/harness/component-helpers.js
// META: script=/wasm/component/jsapi/harness/assertions.js

const HANDLER = "wasi:http/handler@1.0.0";
const PLAIN = "test:names/plain-func";
const NESTED = "test:nested/inner@2.0.0";

promise_test(async () => {
  const log = [];
  const handler = loggingProxy({ handle: x => x + 1 }, log, "handler: ");
  const imports = loggingProxy({ [HANDLER]: handler, [PLAIN]: () => 5 }, log);
  await componentExports("names/interface-names", imports);
  assert_array_equals(log, [`get ${HANDLER}`, "handler: get handle", `get ${PLAIN}`]);
}, "interface names are read verbatim from the imports object");

promise_test(async () => {
  const exports = await componentExports("names/interface-names",
                                         { [HANDLER]: { handle: x => x + 1 }, [PLAIN]: () => 5 });
  assert_array_equals(Reflect.ownKeys(exports), [HANDLER, PLAIN, "outer"]);
  assert_exports_object(exports[HANDLER], ["handle"], HANDLER);
  assert_equals(exports[HANDLER].handle(1), 2);
  assert_equals(exports[PLAIN](), 5);
  assert_exports_object(exports.outer, [NESTED], "outer");
  assert_exports_object(exports.outer[NESTED], ["run"], NESTED);
  assert_equals(exports.outer[NESTED].run(), 5);
}, "interface names are used verbatim as export keys, including nested ones");

promise_test(async () => {
  const exports = await componentExports("names/interface-names",
                                         { [HANDLER]: { handle: x => x }, [PLAIN]: () => 0 });
  for (const wrong of ["wasi:http/handler", "wasiHttpHandler", "handler", "wasi:http/Handler@1.0.0"]) {
    assert_false(Object.hasOwn(exports, wrong), wrong);
  }
}, "interface names are not shortened or re-cased");

promise_test(async t => {
  const imports = { [PLAIN]: () => 0, handler: { handle: x => x } };
  await promise_rejects_js(t, WebAssembly.LinkError,
                           (async () => componentExports("names/interface-names", imports))());
}, "an instance import under a different key is a missing import");
