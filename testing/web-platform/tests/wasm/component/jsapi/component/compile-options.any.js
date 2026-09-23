// META: global=window,dedicatedworker,jsshell
// META: script=/wasm/component/jsapi/harness/component-helpers.js
// META: script=/wasm/component/jsapi/harness/assertions.js

// The `builtins` compile option enables `wasm:js/global`, which resolves to
// globalThis before the imports object is consulted. component/global-echo
// imports `component-test-echo` from it.

const FIXTURE = "component/global-echo";

promise_setup(async () => {
  globalThis.componentTestEcho = x => x + 100;
});

promise_test(async () => {
  const { roundtrip } = await componentExports(FIXTURE, undefined, { builtins: ["js/global"] });
  assert_equals(roundtrip(1), 101);
}, "builtins js/global: resolves against globalThis without an imports object");

promise_test(async () => {
  const log = [];
  const imports = loggingProxy({}, log);
  const { roundtrip } = await componentExports(FIXTURE, imports, { builtins: ["js/global"] });
  assert_equals(roundtrip(2), 102);
  assert_array_equals(log, [], "imports object must not be consulted for a builtin");
}, "builtins js/global: the imports object is not read for a builtin import");

promise_test(async () => {
  const { roundtrip } = await componentExports(FIXTURE, { "wasm:js/global": globalThis });
  assert_equals(roundtrip(3), 103);
}, "builtins disabled: `{ \"wasm:js/global\": globalThis }` is equivalent");

promise_test(async () => {
  const { roundtrip } = await componentExports(FIXTURE, {
    "wasm:js/global": { componentTestEcho: x => x + 200 },
  });
  assert_equals(roundtrip(4), 204);
}, "builtins disabled: the interface name is looked up verbatim on the imports object");

promise_test(async () => {
  const log = [];
  const imports = loggingProxy({ "wasm:js/global": globalThis }, log);
  await componentExports(FIXTURE, imports);
  assert_array_equals(log, ["get wasm:js/global"]);
}, "builtins disabled: exactly one Get for the interface name");

test_instantiation_error(TypeError, FIXTURE, undefined, undefined,
                         "builtins disabled: no imports object throws TypeError");
test_instantiation_error(WebAssembly.LinkError, FIXTURE, {}, undefined,
                         "builtins disabled: missing instance import throws LinkError");
test_instantiation_error(WebAssembly.LinkError, FIXTURE, {}, { builtins: ["bogus"] },
                         "unknown builtin names are ignored");
test_instantiation_error(WebAssembly.LinkError, FIXTURE, {}, { builtins: ["wasm:js/global"] },
                         "the builtin name does not include the `wasm:` prefix");
test_instantiation_error(WebAssembly.LinkError, FIXTURE, {}, { builtins: ["js/Global"] },
                         "builtin names are case-sensitive");
test_instantiation_error(WebAssembly.LinkError, FIXTURE, {}, { builtins: ["js"] },
                         "a prefix of a builtin name is not a builtin");

promise_test(async () => {
  const { roundtrip } = await componentExports(FIXTURE, {}, { builtins: ["bogus", "js/global"] });
  assert_equals(roundtrip(5), 105);
}, "builtins: unknown names alongside a known one are ignored");

promise_test(async () => {
  const { roundtrip } = await componentExports(FIXTURE, {}, { builtins: ["js/global", "js/global"] });
  assert_equals(roundtrip(6), 106);
}, "builtins: duplicate names are accepted");

promise_test(async () => {
  const { roundtrip } = await componentExports(FIXTURE, {}, { builtins: new Set(["js/global"]) });
  assert_equals(roundtrip(7), 107);
}, "builtins: any iterable is accepted");

promise_test(async () => {
  const saved = globalThis.componentTestEcho;
  delete globalThis.componentTestEcho;
  try {
    for (const [, entry] of instantiationEntryPoints) {
      let error;
      try {
        await entry(await componentBytes(FIXTURE), undefined, { builtins: ["js/global"] });
      } catch (e) {
        error = e;
      }
      assert_equals(Object.getPrototypeOf(error), WebAssembly.LinkError.prototype);
    }
  } finally {
    globalThis.componentTestEcho = saved;
  }
}, "builtins js/global: a missing global is a LinkError");

promise_test(async () => {
  const saved = globalThis.componentTestEcho;
  globalThis.componentTestEcho = 42;
  try {
    let error;
    try {
      await instantiateComponent(FIXTURE, undefined, { builtins: ["js/global"] });
    } catch (e) {
      error = e;
    }
    assert_equals(Object.getPrototypeOf(error), WebAssembly.LinkError.prototype);
  } finally {
    globalThis.componentTestEcho = saved;
  }
}, "builtins js/global: a non-callable global is a LinkError");

promise_test(async () => {
  const { roundtrip } = await componentExports(FIXTURE, undefined, { builtins: ["js/global"] });
  globalThis.componentTestEcho = x => x + 300;
  assert_equals(roundtrip(1), 101, "the function was captured at instantiation");
  globalThis.componentTestEcho = x => x + 100;
}, "builtins js/global: the import value is snapshotted at instantiation");
