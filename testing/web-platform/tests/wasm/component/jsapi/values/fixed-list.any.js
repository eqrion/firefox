// META: global=window,dedicatedworker,jsshell
// META: script=/wasm/component/jsapi/harness/component-helpers.js
// META: script=/wasm/component/jsapi/harness/assertions.js

// list<T, N> converts like list<T> and then requires exactly N elements.

let exports;
const received = {};
const imports = {
  echoU32s: x => { received.u32s = x; return x; },
  echoStrings: x => { received.strings = x; return x; },
};
promise_setup(async () => {
  exports = await componentExports("values/fixed-list", imports);
});

promise_test(async () => {
  const result = exports.roundtripU32s([1, 2, 3]);
  assert_true(Array.isArray(result));
  assert_equals(Object.getPrototypeOf(result), Array.prototype);
  assert_equals(result.length, 3);
  assert_value_equals(result, [1, 2, 3], "result");
  assert_value_equals(received.u32s, [1, 2, 3], "value seen by echo");
  assert_equals(received.u32s.length, 3);
}, "list<u32, 3>: an Array of exactly 3 elements roundtrips as an Array of length 3");

promise_test(async () => {
  assert_value_equals(exports.roundtripStrings(["a", "b"]), ["a", "b"], "result");
  assert_value_equals(received.strings, ["a", "b"], "value seen by echo");
}, "list<string, 2>: roundtrips");

promise_test(async () => {
  assert_value_equals(exports.roundtripU32s(new Set([1, 2, 3])), [1, 2, 3], "Set");
  assert_value_equals(exports.roundtripU32s((function* () { yield 1; yield 2; yield 3; })()), [1, 2, 3],
                      "generator");
  assert_value_equals(exports.roundtripU32s(["1", true, null]), [1, 1, 0], "coercible elements");
}, "list<u32, 3>: any iterable of the right length converts");

for (const [input, description] of [
  [[], "an empty Array"],
  [[1, 2], "2 elements"],
  [[1, 2, 3, 4], "4 elements"],
  [new Set([1, 1, 1]), "a Set that dedupes to 1 element"],
]) {
  promise_test(async () => {
    received.u32s = "unchanged";
    assert_throws_js(TypeError, () => exports.roundtripU32s(input));
    assert_equals(received.u32s, "unchanged", "echo must not be called");
  }, `list<u32, 3>: ${description} throws TypeError`);
}

promise_test(async () => {
  assert_throws_js(TypeError, () => exports.roundtripStrings(["a"]));
  assert_throws_js(TypeError, () => exports.roundtripStrings(["a", "b", "c"]));
}, "list<string, 2>: wrong lengths throw TypeError");

promise_test(async () => {
  assert_throws_js(TypeError, () => exports.roundtripU32s([1, -1, 3]));
  assert_throws_js(TypeError, () => exports.roundtripU32s("abc"));
  assert_throws_js(TypeError, () => exports.roundtripU32s({ length: 3 }));
}, "list<u32, 3>: element and iterability errors apply as for list<u32>");

promise_test(async () => {
  const visited = [];
  const iterable = {
    *[Symbol.iterator]() {
      for (const v of [1, 2, 3, 4, 5]) {
        visited.push(v);
        yield v;
      }
    },
  };
  assert_throws_js(TypeError, () => exports.roundtripU32s(iterable));
  assert_array_equals(visited, [1, 2, 3, 4, 5], "the whole iterable is consumed before the length check");
}, "list<u32, 3>: the length is checked after conversion");

promise_test(async () => {
  const e = await componentExports("values/fixed-list", { ...imports, echoU32s: () => new Set([4, 5, 6]) });
  assert_value_equals(e.roundtripU32s([1, 2, 3]), [4, 5, 6], "result");
}, "list<u32, 3>: an import returning an iterable of the right length is converted");

for (const [value, description] of [
  [[1, 2], "2 elements"],
  [[1, 2, 3, 4], "4 elements"],
  [5, "a Number"],
]) {
  promise_test(async () => {
    const e = await componentExports("values/fixed-list", { ...imports, echoU32s: () => value });
    assert_traps(() => e.roundtripU32s([1, 2, 3]));
  }, `list<u32, 3>: an import returning ${description} traps`);
}
