// META: global=window,dedicatedworker,jsshell
// META: script=/wasm/component/jsapi/harness/assertions.js

function test_interface(name, length, parent = Function.prototype) {
  const iface = WebAssembly[name];

  test(() => {
    const propdesc = Object.getOwnPropertyDescriptor(WebAssembly, name);
    assert_equals(typeof propdesc, "object");
    assert_true(propdesc.writable, "writable");
    assert_false(propdesc.enumerable, "enumerable");
    assert_true(propdesc.configurable, "configurable");
    assert_equals(propdesc.value, iface);
  }, `WebAssembly.${name}: property descriptor`);

  test(() => {
    assert_equals(typeof iface, "function");
    assert_equals(Object.getPrototypeOf(iface), parent);
  }, `WebAssembly.${name}: is a function`);

  test(() => {
    assert_function_name(iface, name, `WebAssembly.${name}`);
  }, `WebAssembly.${name}: name`);

  test(() => {
    assert_function_length(iface, length, `WebAssembly.${name}`);
  }, `WebAssembly.${name}: length`);

  test(() => {
    assert_throws_js(TypeError, () => iface());
  }, `WebAssembly.${name}: calling without new`);

  test(() => {
    const propdesc = Object.getOwnPropertyDescriptor(iface, "prototype");
    assert_equals(typeof propdesc, "object");
    assert_false(propdesc.writable, "writable");
    assert_false(propdesc.enumerable, "enumerable");
    assert_false(propdesc.configurable, "configurable");
  }, `WebAssembly.${name}.prototype: property descriptor`);

  test(() => {
    const propdesc = Object.getOwnPropertyDescriptor(iface.prototype, "constructor");
    assert_equals(typeof propdesc, "object");
    assert_true(propdesc.writable, "writable");
    assert_false(propdesc.enumerable, "enumerable");
    assert_true(propdesc.configurable, "configurable");
    assert_equals(propdesc.value, iface);
  }, `WebAssembly.${name}.prototype.constructor`);
}

function test_toStringTag(name) {
  test(() => {
    const propdesc = Object.getOwnPropertyDescriptor(WebAssembly[name].prototype, Symbol.toStringTag);
    assert_equals(typeof propdesc, "object");
    assert_false(propdesc.writable, "writable");
    assert_false(propdesc.enumerable, "enumerable");
    assert_true(propdesc.configurable, "configurable");
    assert_equals(propdesc.value, `WebAssembly.${name}`);
  }, `WebAssembly.${name}.prototype[Symbol.toStringTag]`);
}

test_interface("Component", 1);
test_toStringTag("Component");
test(() => {
  assert_equals(Object.getPrototypeOf(WebAssembly.Component.prototype), Object.prototype);
}, "WebAssembly.Component.prototype: prototype");

test_interface("ComponentInstance", 1);
test_toStringTag("ComponentInstance");
test(() => {
  assert_equals(Object.getPrototypeOf(WebAssembly.ComponentInstance.prototype), Object.prototype);
}, "WebAssembly.ComponentInstance.prototype: prototype");

test(() => {
  const propdesc = Object.getOwnPropertyDescriptor(WebAssembly.ComponentInstance.prototype, "exports");
  assert_equals(typeof propdesc, "object");
  assert_true(propdesc.enumerable, "enumerable");
  assert_true(propdesc.configurable, "configurable");
  assert_equals(typeof propdesc.get, "function");
  assert_function_name(propdesc.get, "get exports", "getter for exports");
  assert_function_length(propdesc.get, 0, "getter for exports");
  assert_equals(propdesc.set, undefined, "no setter");
}, "WebAssembly.ComponentInstance.prototype.exports: property descriptor");

test(() => {
  const getter = Object.getOwnPropertyDescriptor(WebAssembly.ComponentInstance.prototype, "exports").get;
  for (const thisValue of [undefined, null, {}, WebAssembly.ComponentInstance.prototype,
                           new WebAssembly.Instance(new WebAssembly.Module(
                             new Uint8Array([0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00])))]) {
    assert_throws_js(TypeError, () => getter.call(thisValue), `this = ${format_value(thisValue)}`);
  }
}, "WebAssembly.ComponentInstance.prototype.exports: brand check");

test_interface("ComponentError", 0, Error);
test(() => {
  assert_equals(Object.getPrototypeOf(WebAssembly.ComponentError.prototype), Error.prototype);
}, "WebAssembly.ComponentError.prototype: prototype");
test(() => {
  assert_false(Object.hasOwn(WebAssembly.ComponentError.prototype, Symbol.toStringTag));
}, "WebAssembly.ComponentError.prototype: no Symbol.toStringTag");
test(() => {
  const propdesc = Object.getOwnPropertyDescriptor(WebAssembly.ComponentError.prototype, "name");
  assert_equals(typeof propdesc, "object");
  assert_true(propdesc.writable, "writable");
  assert_false(propdesc.enumerable, "enumerable");
  assert_true(propdesc.configurable, "configurable");
  assert_equals(propdesc.value, "ComponentError");
}, "WebAssembly.ComponentError.prototype.name");
test(() => {
  const propdesc = Object.getOwnPropertyDescriptor(WebAssembly.ComponentError.prototype, "data");
  assert_equals(typeof propdesc, "object");
  assert_true(propdesc.enumerable, "enumerable");
  assert_true(propdesc.configurable, "configurable");
  assert_function_name(propdesc.get, "get data", "getter for data");
  assert_function_length(propdesc.get, 0, "getter for data");
  assert_equals(propdesc.set, undefined, "no setter");
}, "WebAssembly.ComponentError.prototype.data: property descriptor");

test(() => {
  const error = new WebAssembly.ComponentError();
  assert_equals(Object.getPrototypeOf(error), WebAssembly.ComponentError.prototype);
  assert_true(error instanceof Error);
  assert_equals(error.message, "");
  assert_equals(error.data, undefined);
  assert_false(Object.hasOwn(error, "data"), "data is not an own property");
}, "new WebAssembly.ComponentError()");

test(() => {
  const data = { a: 1 };
  const error = new WebAssembly.ComponentError("failed", data);
  assert_equals(error.message, "failed");
  assert_equals(error.data, data);
  assert_equals(String(error), "ComponentError: failed");
}, "new WebAssembly.ComponentError(message, data)");

test(() => {
  const error = new WebAssembly.ComponentError({ toString() { return "coerced"; } }, 42);
  assert_equals(error.message, "coerced");
  assert_equals(error.data, 42);
  assert_throws_js(TypeError, () => new WebAssembly.ComponentError(Symbol()));
}, "new WebAssembly.ComponentError: message is stringified");

test(() => {
  const getter = Object.getOwnPropertyDescriptor(WebAssembly.ComponentError.prototype, "data").get;
  for (const thisValue of [undefined, null, {}, new Error(), new WebAssembly.RuntimeError()]) {
    assert_throws_js(TypeError, () => getter.call(thisValue), `this = ${format_value(thisValue)}`);
  }
}, "WebAssembly.ComponentError.prototype.data: brand check");

test(() => {
  assert_function_length(WebAssembly.validate, 1, "WebAssembly.validate");
  assert_function_length(WebAssembly.compile, 1, "WebAssembly.compile");
  assert_function_length(WebAssembly.instantiate, 1, "WebAssembly.instantiate");
}, "WebAssembly namespace operations keep their lengths");
