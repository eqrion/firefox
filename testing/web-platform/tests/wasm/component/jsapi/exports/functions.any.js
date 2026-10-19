// META: global=window,dedicatedworker,jsshell
// META: script=/wasm/component/jsapi/harness/component-helpers.js
// META: script=/wasm/component/jsapi/harness/assertions.js

let exports;
promise_setup(async () => {
  exports = await componentExports("exports/functions");
});

promise_test(async () => {
  assert_array_equals(Reflect.ownKeys(exports), ["nullary", "addOne", "sum3"]);
}, "exported function keys");

promise_test(async () => {
  assert_builtin_function(exports.nullary, { name: "nullary", length: 0 }, "nullary");
  assert_builtin_function(exports.addOne, { name: "addOne", length: 1 }, "addOne");
  assert_builtin_function(exports.sum3, { name: "sum3", length: 3 }, "sum3");
}, "exported functions are built-in functions with the JS name and the parameter count");

promise_test(async () => {
  assert_equals(exports.nullary(), undefined);
  assert_equals(exports.addOne(1), 2);
  assert_equals(exports.sum3(1, 2, 3), 6);
}, "exported functions are callable");

promise_test(async () => {
  const { addOne, sum3 } = exports;
  assert_equals(addOne(1), 2, "extracted");
  assert_equals(addOne.call({}, 1), 2, "object this");
  assert_equals(addOne.call(null, 1), 2, "null this");
  assert_equals(Reflect.apply(sum3, "string", [1, 2, 3]), 6, "primitive this");
  assert_equals(addOne.bind(undefined, 4)(), 5, "bound");
}, "the this value is ignored");

promise_test(async () => {
  assert_equals(exports.addOne(), 1);
  assert_equals(exports.sum3(1, 2), 3);
}, "missing arguments are undefined");

promise_test(async () => {
  assert_equals(exports.nullary(1, 2, 3), undefined);
  assert_equals(exports.sum3(1, 2, 3, 100), 6);
}, "extra arguments are ignored");

promise_test(async () => {
  assert_equals(exports.addOne.prototype, undefined);
  assert_throws_js(TypeError, () => new exports.addOne(1));
  assert_throws_js(TypeError, () => Reflect.construct(exports.nullary, []));
}, "exported functions are not constructors");

promise_test(async () => {
  assert_false(Object.hasOwn(exports.addOne, "caller"));
  assert_false(Object.hasOwn(exports.addOne, "arguments"));
  assert_array_equals(Reflect.ownKeys(exports.addOne).map(String).sort(), ["length", "name"]);
}, "exported functions have no extra own properties");
