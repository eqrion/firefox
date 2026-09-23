// META: global=window,dedicatedworker,jsshell
// META: script=/wasm/component/jsapi/harness/component-helpers.js
// META: script=/wasm/component/jsapi/harness/assertions.js

// WebAssembly.compile dispatches on the `layer` field of the binary header.

promise_test(async () => {
  const component = await WebAssembly.compile(await componentBytes("constructor/no-imports"));
  assert_equals(Object.getPrototypeOf(component), WebAssembly.Component.prototype);
  const instance = new WebAssembly.ComponentInstance(component);
  assert_equals(instance.exports.answer(), 42);
}, "compile: component bytes resolve to a Component");

promise_test(async () => {
  const module = await WebAssembly.compile(emptyModuleBytes);
  assert_equals(Object.getPrototypeOf(module), WebAssembly.Module.prototype);
}, "compile: core module bytes still resolve to a Module");

promise_test(async () => {
  const component = await WebAssembly.compile(emptyComponentBytes);
  assert_equals(Object.getPrototypeOf(component), WebAssembly.Component.prototype);
  assert_exports_object(new WebAssembly.ComponentInstance(component).exports, []);
}, "compile: an empty component resolves to a Component");

promise_test(async t => {
  await promise_rejects_js(t, WebAssembly.CompileError,
                           WebAssembly.compile(new Uint8Array([...emptyComponentBytes, 0xff, 0xff])));
}, "compile: a malformed component rejects with CompileError");

promise_test(async t => {
  const bytes = await componentBytesCopy("constructor/no-imports");
  bytes[bytes.length - 1] ^= 0xff;
  await promise_rejects_js(t, WebAssembly.CompileError, WebAssembly.compile(bytes));
}, "compile: a corrupted component rejects with CompileError");

promise_test(async t => {
  await promise_rejects_js(t, WebAssembly.CompileError, WebAssembly.compile(new Uint8Array()));
}, "compile: empty bytes reject with CompileError");

promise_test(async t => {
  for (const argument of [undefined, null, true, "", Symbol(), 1, {}, []]) {
    await promise_rejects_js(t, TypeError, WebAssembly.compile(argument),
                             `compile(${format_value(argument)})`);
  }
}, "compile: non-BufferSource arguments reject with TypeError");

promise_test(async t => {
  const bytes = await componentBytes("constructor/no-imports");
  await promise_rejects_js(t, TypeError, WebAssembly.compile(bytes, { builtins: 1 }));
  await promise_rejects_js(t, TypeError, WebAssembly.compile(bytes, 1));
  const component = await WebAssembly.compile(bytes, { builtins: ["js/global"] });
  assert_equals(Object.getPrototypeOf(component), WebAssembly.Component.prototype);
}, "compile: compile options are parsed");

promise_test(async () => {
  const bytes = await componentBytesCopy("constructor/no-imports");
  const promise = WebAssembly.compile(bytes);
  bytes.fill(0);
  const component = await promise;
  assert_equals(new WebAssembly.ComponentInstance(component).exports.answer(), 42);
}, "compile: bytes are copied before compilation");

promise_test(async () => {
  const bytes = await componentBytes("constructor/no-imports");
  const promise = WebAssembly.compile(bytes);
  assert_true(promise instanceof Promise);
  assert_equals(Object.getPrototypeOf(promise), Promise.prototype);
  await promise;
}, "compile: returns a Promise");
