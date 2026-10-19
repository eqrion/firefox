// META: global=window,dedicatedworker,jsshell
// META: script=/wasm/component/jsapi/harness/component-helpers.js
// META: script=/wasm/component/jsapi/harness/assertions.js

const enabled = { builtins: ["js/global"] };

promise_test(async () => {
  const { encode, decode } = await componentExports("imports/builtins-global", {}, enabled);
  assert_equals(encode("hello"), btoa("hello"));
  assert_equals(decode(btoa("hello")), "hello");
}, "plain functions are read from the global object");

promise_test(async () => {
  const { urlJson, canParse } = await componentExports("imports/builtins-global", {}, enabled);
  assert_equals(urlJson("https://example.com/a?b#c"), new URL("https://example.com/a?b#c").toJSON());
  assert_true(canParse("https://example.com/"));
  assert_false(canParse("not a url"));
}, "a WebIDL interface is imported as a resource with a constructor, method and static");

promise_test(async () => {
  const { mapRoundtrip } = await componentExports("imports/builtins-global", {}, enabled);
  assert_equals(mapRoundtrip("k", "v"), "v");
}, "a JS class is imported as a resource and its methods are invoked on the instance");

promise_test(async () => {
  const { urlJson } = await componentExports("imports/builtins-global", {}, enabled);
  assert_throws_js(WebAssembly.RuntimeError, () => urlJson("not a url"),
                   "the constructor throws a TypeError which traps for a non-result type");
}, "a throwing WebIDL constructor traps when the import type is not a result");

promise_test(async () => {
  const { encode } = await componentExports("imports/builtins-global", undefined, enabled);
  assert_equals(encode("x"), btoa("x"));
}, "an enabled builtin needs no imports object");

promise_test(async () => {
  const log = [];
  const imports = loggingProxy({ "wasm:js/global": { btoa: () => "never" } }, log);
  const { encode } = await componentExports("imports/builtins-global", imports, enabled);
  assert_equals(encode("x"), btoa("x"));
  assert_array_equals(log, []);
}, "an enabled builtin is not read from the imports object");

promise_test(async () => {
  const fake = {
    btoa: s => `b:${s}`, atob: s => `a:${s}`,
    URL: class { constructor(u) { this.u = u; } toJSON() { return `j:${this.u}`; } static canParse() { return false; } },
    Map,
  };
  const { encode, urlJson, canParse } = await componentExports("imports/builtins-global", { "wasm:js/global": fake });
  assert_equals(encode("x"), "b:x");
  assert_equals(urlJson("u"), "j:u");
  assert_false(canParse("https://example.com/"));
}, "when not enabled, wasm:js/global is an ordinary import keyed by the interface name");

promise_test(async () => {
  const { encode } = await componentExports("imports/builtins-global", { "wasm:js/global": globalThis });
  assert_equals(encode("x"), btoa("x"));
}, "passing globalThis under the wasm:js/global key is equivalent to enabling the builtin");

test_instantiation_error(WebAssembly.LinkError, "imports/builtins-global", {}, undefined,
                         "a disabled builtin with no matching import is a LinkError");
test_instantiation_error(WebAssembly.LinkError, "imports/builtins-global-missing", {}, enabled,
                         "a global function that does not exist is a LinkError");
test_instantiation_error(WebAssembly.LinkError, "imports/builtins-global", {}, { builtins: ["js-global", "global"] },
                         "misspelled builtin names do not enable wasm:js/global");

promise_test(async () => {
  const { encode } = await componentExports("imports/builtins-global", {}, { builtins: ["js/global", "no-such-builtin"] });
  assert_equals(encode("x"), btoa("x"));
}, "unknown builtin names are ignored");

promise_test(async () => {
  const { encode } = await componentExports("imports/builtins-global", {}, { builtins: new Set(["js/global"]) });
  assert_equals(encode("x"), btoa("x"));
}, "builtins may be any iterable of strings");

promise_test(async t => {
  const bytes = await componentBytes("imports/builtins-global");
  for (const builtins of [42, {}, null, true]) {
    assert_throws_js(TypeError, () => new WebAssembly.Component(bytes, { builtins }), format_value(builtins));
    await promise_rejects_js(t, TypeError, WebAssembly.compile(bytes, { builtins }), format_value(builtins));
  }
}, "a non-iterable builtins option is a TypeError");

promise_test(async () => {
  const { href } = await componentExports("imports/builtins-global-href", {}, enabled);
  assert_equals(href("https://example.com/path"), "https://example.com/path");
}, "a [method][get] on a WebIDL interface reads the attribute getter");

promise_test(async () => {
  const saved = globalThis.btoa;
  globalThis.btoa = s => `patched:${s}`;
  try {
    const { encode } = await componentExports("imports/builtins-global", {}, enabled);
    assert_equals(encode("x"), "patched:x");
  } finally {
    globalThis.btoa = saved;
  }
}, "wasm:js/global reads the current value of the global property at instantiation");

promise_test(async t => {
  const log = [];
  globalThis["component-projection"] = {
    value: () => 1,
    get nested() { log.push("get nested"); return { value: () => 2 }; },
  };
  t.add_cleanup(() => { delete globalThis["component-projection"]; });
  const { instanceValue, nestedValue } =
    await componentExports("imports/builtins-global-projection", undefined, enabled);
  assert_array_equals(log, ["get nested"], "each projection is read with Get at instantiation");
  assert_equals(instanceValue(), 1, "wasm:js/global/component-projection");
  assert_equals(nestedValue(), 2, "wasm:js/global/component-projection/nested/value");
}, "a path below wasm:js/global projects properties off of the global object");

promise_test(async t => {
  globalThis["component-projection"] = { value: () => 1 };
  t.add_cleanup(() => { delete globalThis["component-projection"]; });
  await promise_rejects_js(t, TypeError,
                           componentExports("imports/builtins-global-projection", undefined, enabled));
}, "a projection through a missing property throws TypeError");

promise_test(async () => {
  const imports = {
    "wasm:js/global/component-projection": { value: () => 3 },
    "wasm:js/global/component-projection/nested/value": () => 4,
  };
  const { instanceValue, nestedValue } = await componentExports("imports/builtins-global-projection", imports);
  assert_equals(instanceValue(), 3);
  assert_equals(nestedValue(), 4);
}, "when not enabled, paths below wasm:js/global are ordinary imports keyed by the interface name");
