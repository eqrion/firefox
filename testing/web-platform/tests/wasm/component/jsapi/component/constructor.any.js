// META: global=window,dedicatedworker,jsshell
// META: script=/wasm/component/jsapi/harness/component-helpers.js
// META: script=/wasm/component/jsapi/harness/assertions.js

// new WebAssembly.Component(bytes, options) compiles synchronously.

promise_test(async () => {
  const component = new WebAssembly.Component(await componentBytes("constructor/no-imports"));
  assert_equals(Object.getPrototypeOf(component), WebAssembly.Component.prototype);
  assert_true(Object.isExtensible(component));
  assert_equals(Object.prototype.toString.call(component), "[object WebAssembly.Component]");
}, "new Component: a valid component");

test(() => {
  const component = new WebAssembly.Component(emptyComponentBytes);
  assert_equals(Object.getPrototypeOf(component), WebAssembly.Component.prototype);
}, "new Component: an empty component");

test(() => {
  assert_throws_js(TypeError, () => new WebAssembly.Component());
}, "new Component: no arguments throws TypeError");

test(() => {
  for (const argument of [undefined, null, true, "", Symbol(), 1, {}, [], WebAssembly.Module]) {
    assert_throws_js(TypeError, () => new WebAssembly.Component(argument),
                     `new Component(${format_value(argument)})`);
  }
}, "new Component: non-BufferSource arguments throw TypeError");

test(() => {
  assert_throws_js(WebAssembly.CompileError, () => new WebAssembly.Component(emptyModuleBytes));
}, "new Component: core module bytes throw CompileError");

test(() => {
  assert_throws_js(WebAssembly.CompileError, () => new WebAssembly.Module(emptyComponentBytes));
}, "new Module: component bytes throw CompileError");

test(() => {
  assert_throws_js(WebAssembly.CompileError,
                   () => new WebAssembly.Component(new Uint8Array([...emptyComponentBytes, 0xff, 0xff])));
}, "new Component: a malformed component throws CompileError");

test(() => {
  assert_throws_js(WebAssembly.CompileError, () => new WebAssembly.Component(new Uint8Array()));
}, "new Component: empty bytes throw CompileError");

promise_test(async () => {
  const bytes = await componentBytesCopy("constructor/no-imports");
  bytes[bytes.length - 1] ^= 0xff;
  assert_throws_js(WebAssembly.CompileError, () => new WebAssembly.Component(bytes));
}, "new Component: a corrupted component throws CompileError");

promise_test(async () => {
  const bytes = await componentBytesCopy("constructor/no-imports");
  const component = new WebAssembly.Component(bytes);
  bytes.fill(0);
  assert_equals(new WebAssembly.ComponentInstance(component).exports.answer(), 42);
}, "new Component: bytes are copied");

promise_test(async () => {
  const bytes = await componentBytes("constructor/no-imports");
  for (const view of [bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength),
                      new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength),
                      new Int8Array(bytes.buffer, bytes.byteOffset, bytes.byteLength)]) {
    const component = new WebAssembly.Component(view);
    assert_equals(new WebAssembly.ComponentInstance(component).exports.answer(), 42, format_value(view));
  }
}, "new Component: ArrayBuffer and any ArrayBufferView are accepted");

promise_test(async () => {
  const bytes = await componentBytes("constructor/no-imports");
  const buffer = new ArrayBuffer(bytes.length, { maxByteLength: bytes.length * 2 });
  new Uint8Array(buffer).set(bytes);
  const component = new WebAssembly.Component(buffer);
  buffer.resize(bytes.length * 2);
  assert_equals(new WebAssembly.ComponentInstance(component).exports.answer(), 42);
}, "new Component: resizable ArrayBuffer is accepted and copied");

promise_test(async () => {
  if (typeof SharedArrayBuffer === "undefined") {
    return;
  }
  const bytes = await componentBytes("constructor/no-imports");
  const buffer = new SharedArrayBuffer(bytes.length);
  new Uint8Array(buffer).set(bytes);
  const component = new WebAssembly.Component(buffer);
  assert_equals(new WebAssembly.ComponentInstance(component).exports.answer(), 42);
}, "new Component: SharedArrayBuffer is accepted");

promise_test(async () => {
  const bytes = await componentBytes("constructor/no-imports");
  for (const options of [undefined, null, {}, { builtins: [] }, { builtins: ["js/global"] },
                         { builtins: ["bogus", "js/global"] }, { unrelated: 1 }]) {
    const component = new WebAssembly.Component(bytes, options);
    assert_equals(Object.getPrototypeOf(component), WebAssembly.Component.prototype,
                  `options ${format_value(options)}`);
  }
}, "new Component: options dictionary variants are accepted");

promise_test(async () => {
  const bytes = await componentBytes("constructor/no-imports");
  for (const options of [1, "", true, Symbol()]) {
    assert_throws_js(TypeError, () => new WebAssembly.Component(bytes, options),
                     `options ${format_value(options)}`);
  }
  for (const builtins of [1, true, {}, Symbol(), [Symbol()], "js/global"]) {
    assert_throws_js(TypeError, () => new WebAssembly.Component(bytes, { builtins }),
                     `builtins ${format_value(builtins)}`);
  }
}, "new Component: bad options throw");

promise_test(async () => {
  const bytes = await componentBytes("constructor/no-imports");
  const order = [];
  const options = {
    get builtins() {
      order.push("builtins");
      return { [Symbol.iterator]() { order.push("iterator"); return [][Symbol.iterator](); } };
    },
  };
  new WebAssembly.Component(bytes, options);
  assert_array_equals(order, ["builtins", "iterator"]);
}, "new Component: builtins is read and iterated once");

promise_test(async () => {
  const bytes = await componentBytes("constructor/no-imports");
  const component = Reflect.construct(WebAssembly.Component, [bytes], WebAssembly.Module);
  assert_equals(Object.getPrototypeOf(component), WebAssembly.Module.prototype);
  assert_equals(new WebAssembly.ComponentInstance(component).exports.answer(), 42);
}, "new Component: newTarget.prototype is used");

promise_test(async () => {
  const bytes = await componentBytes("constructor/no-imports");
  const component = Reflect.construct(WebAssembly.Component, [bytes],
                                      function () {}.bind());
  assert_equals(Object.getPrototypeOf(component), WebAssembly.Component.prototype);
}, "new Component: a newTarget without an object prototype falls back to Component.prototype");
