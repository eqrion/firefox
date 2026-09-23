// META: global=window,dedicatedworker,jsshell
// META: script=/wasm/component/jsapi/harness/component-helpers.js
// META: script=/wasm/component/jsapi/harness/assertions.js

// The JS-API reserves these names on both the class and its prototype. It
// will export such members through the exports object instead, once it gives
// them a JS name there; until then they are rejected.
for (const [scope, name] of [
  ["method", "constructor"],
  ["static", "prototype"],
  ["method", "prototype"],
  ["static", "constructor"],
  ["method", "name"],
  ["static", "length"],
]) {
  test_instantiation_error(TypeError, `names/reserved-${scope}-${name}`, undefined, undefined,
                           `a [${scope}] named \`${name}\` throws a TypeError`);
}

promise_test(async () => {
  let receiver;
  function R() {}
  R.prototype = function proto() { return 7; };
  R.prototype.constructor = function ctor() { receiver = this; return 5; };
  const { callConstructor, callPrototype } = await componentExports("names/reserved-imports", { R });
  const r = new R();
  assert_equals(callConstructor(r), 5, "[method]r.constructor reads R.prototype.constructor");
  assert_equals(receiver, r, "and calls it with self as this");
  assert_equals(callPrototype(), 7, "[static]r.prototype reads R.prototype");
}, "`constructor` and `prototype` are not reserved for imports");
