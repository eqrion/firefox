// META: global=window,dedicatedworker,jsshell
// META: script=/wasm/component/jsapi/harness/component-helpers.js
// META: script=/wasm/component/jsapi/harness/assertions.js

// Value imports: greeting: string, port: u16, tags: list<string>,
// limit: option<u32>, re-exported as default-greeting, default-port,
// default-tags and default-limit.

const imports = { greeting: "hi", port: 8080, tags: ["a", "b"], limit: 5 };

promise_test(async () => {
  const instance = await instantiateComponent("values/value-imports-exports", imports);
  assert_ComponentInstance(instance, ["defaultGreeting", "defaultPort", "defaultTags", "defaultLimit"]);
  const { exports } = instance;
  assert_equals(exports.defaultGreeting, "hi");
  assert_equals(exports.defaultPort, 8080);
  assert_value_equals(exports.defaultTags, ["a", "b"]);
  assert_equals(exports.defaultLimit, 5);
}, "value exports are data properties holding the converted values");

promise_test(async () => {
  const { exports } = await instantiateComponent("values/value-imports-exports", imports);
  assert_equals(exports.defaultTags, exports.defaultTags, "the same array every read");
  assert_not_equals(exports.defaultTags, imports.tags, "a fresh array, not the imported one");
  const desc = Object.getOwnPropertyDescriptor(exports, "defaultTags");
  assert_true(Object.hasOwn(desc, "value"), "data property");
}, "a value export is converted once at instantiation");

promise_test(async () => {
  const { exports } = await instantiateComponent("values/value-imports-exports", {
    greeting: 42, port: "80", tags: new Set(["x"]), limit: null,
  });
  assert_equals(exports.defaultGreeting, "42");
  assert_equals(exports.defaultPort, 80);
  assert_value_equals(exports.defaultTags, ["x"]);
  assert_equals(exports.defaultLimit, null);
}, "value imports are converted with ToComponentValue");

promise_test(async () => {
  const { exports } = await instantiateComponent("values/value-imports-exports", { ...imports, greeting: undefined, limit: undefined });
  assert_equals(exports.defaultGreeting, "undefined", "a missing string value is ToString(undefined)");
  assert_equals(exports.defaultLimit, null, "a missing option value is none");
}, "a missing value import is converted like undefined");

for (const [bad, description] of [
  [{ ...imports, port: -1 }, "a negative u16"],
  [{ ...imports, port: 70000 }, "an out-of-range u16"],
  [{ ...imports, port: undefined }, "a missing u16"],
  [{ ...imports, port: NaN }, "NaN for u16"],
  [{ ...imports, tags: undefined }, "a missing list"],
  [{ ...imports, tags: "ab" }, "a string for a list"],
  [{ ...imports, tags: [Symbol()] }, "an unconvertible list element"],
  [{ ...imports, limit: "x" }, "an unconvertible option payload"],
]) {
  test_instantiation_error(TypeError, "values/value-imports-exports", bad, undefined,
                           `value import: ${description} throws TypeError`);
}

promise_test(async () => {
  const log = [];
  await instantiateComponent("values/value-imports-exports", loggingProxy(imports, log));
  assert_array_equals(log, ["get greeting", "get port", "get tags", "get limit"]);
}, "value imports are read once each, in declaration order");

promise_test(async t => {
  const log = [];
  const bad = loggingProxy({ ...imports, port: -1 }, log);
  await promise_rejects_js(t, TypeError, (async () => instantiateComponent("values/value-imports-exports", bad))());
  assert_array_equals(log, ["get greeting", "get port"], "reading stops at the failing import");
}, "a failing value import stops reading later imports");
