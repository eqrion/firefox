// META: global=window,dedicatedworker,jsshell
// META: script=/wasm/component/jsapi/harness/component-helpers.js
// META: script=/wasm/component/jsapi/harness/assertions.js

let exports;
promise_setup(async () => {
  exports = await componentExports("exports/core-modules");
});

promise_test(async () => {
  assert_array_equals(Reflect.ownKeys(exports), ["adder", "myImporter"]);
}, "core module exports are keyed by their CamelCase name");

promise_test(async () => {
  assert_equals(Object.getPrototypeOf(exports.adder), WebAssembly.Module.prototype);
  assert_true(exports.adder instanceof WebAssembly.Module);
  assert_true(Object.isExtensible(exports.adder));
}, "an exported core module is a WebAssembly.Module");

promise_test(async () => {
  const moduleExports = WebAssembly.Module.exports(exports.adder);
  assert_equals(moduleExports.length, 2);
  assert_equals(moduleExports[0].name, "mem");
  assert_equals(moduleExports[0].kind, "memory");
  assert_equals(moduleExports[1].name, "add");
  assert_equals(moduleExports[1].kind, "function");
  const moduleImports = WebAssembly.Module.imports(exports.myImporter);
  assert_equals(moduleImports.length, 1);
  assert_equals(moduleImports[0].module, "env");
  assert_equals(moduleImports[0].name, "cb");
  assert_equals(moduleImports[0].kind, "function");
}, "module reflection works on exported core modules");

promise_test(async () => {
  const instance = new WebAssembly.Instance(exports.adder);
  assert_equals(instance.exports.add(2, 3), 5);
  const importer = new WebAssembly.Instance(exports.myImporter, { env: { cb: () => 9 } });
  assert_equals(importer.exports.run(), 9);
  const { instance: viaPromise } = await WebAssembly.instantiate(exports.adder);
  assert_equals(viaPromise.exports.add(1, 1), 2);
}, "exported core modules can be instantiated by JS");

promise_test(async () => {
  assert_equals(exports.adder, exports.adder, "same read");
  const other = await componentExports("exports/core-modules");
  assert_not_equals(other.adder, exports.adder, "another instantiation");
}, "each instantiation wraps the core module in a fresh Module object");
