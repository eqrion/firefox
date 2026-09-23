// META: global=window,dedicatedworker,jsshell
// META: script=/wasm/component/jsapi/harness/component-helpers.js
// META: script=/wasm/component/jsapi/harness/assertions.js

// An exported component definition becomes a WebAssembly.Component on the
// exports object.

promise_test(async () => {
  const { inner } = await componentExports("nested/component-export");
  assert_equals(Object.getPrototypeOf(inner), WebAssembly.Component.prototype);
  assert_true(inner instanceof WebAssembly.Component);
}, "an exported component is a WebAssembly.Component");

promise_test(async () => {
  const instance = await instantiateComponent("nested/component-export");
  assert_ComponentInstance(instance, ["inner"]);
}, "the exports object holds the component as a frozen data property");

promise_test(async () => {
  const { inner } = await componentExports("nested/component-export");
  const instance = new WebAssembly.ComponentInstance(inner);
  assert_ComponentInstance(instance, ["next"]);
  assert_equals(instance.exports.next(), 1);
  assert_equals(instance.exports.next(), 2);
}, "an exported component can be instantiated with new ComponentInstance");

promise_test(async () => {
  const { inner } = await componentExports("nested/component-export");
  const instance = await WebAssembly.instantiate(inner);
  assert_equals(Object.getPrototypeOf(instance), WebAssembly.ComponentInstance.prototype);
  assert_equals(instance.exports.next(), 1);
}, "an exported component can be instantiated with WebAssembly.instantiate");

promise_test(async () => {
  const { inner } = await componentExports("nested/component-export");
  const a = new WebAssembly.ComponentInstance(inner);
  const b = new WebAssembly.ComponentInstance(inner);
  assert_equals(a.exports.next(), 1);
  assert_equals(a.exports.next(), 2);
  assert_equals(b.exports.next(), 1);
}, "instances of an exported component have independent state");

promise_test(async () => {
  const a = await componentExports("nested/component-export");
  const b = await componentExports("nested/component-export");
  assert_not_equals(a.inner, b.inner);
}, "each instantiation creates a fresh Component object");
