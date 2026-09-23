// META: global=window,dedicatedworker,jsshell
// META: script=/wasm/component/jsapi/harness/component-helpers.js
// META: script=/wasm/component/jsapi/harness/assertions.js

// map<string, u32>, map<s64, string>, map<u32, string>

let exports;
let received;
const echo = x => { received = x; return x; };
const imports = { echoStringU32: echo, echoS64String: echo, echoU32String: echo };
promise_setup(async () => {
  exports = await componentExports("values/map", imports);
});

promise_test(async () => {
  const input = new Map([["b", 1], ["a", 2], ["c", 3]]);
  const result = exports.roundtripStringU32(input);
  assert_value_equals(received, input, "value seen by echo");
  assert_value_equals(result, input);
  assert_not_equals(result, input, "a fresh Map is created");
  assert_equals(Object.getPrototypeOf(result), Map.prototype);
}, "map: a Map roundtrips in insertion order");

promise_test(async () => {
  assert_value_equals(exports.roundtripStringU32(new Map()), new Map());
  assert_value_equals(exports.roundtripStringU32([]), new Map());
}, "map: empty");

for (const [input, description] of [
  [[["x", 1], ["y", 2]], "an array of pairs"],
  [new Set([["x", 1], ["y", 2]]), "a Set of pairs"],
  [(function* () { yield ["x", 1]; yield ["y", 2]; })(), "a generator of pairs"],
  [[new Map([["x", 1]]).entries().next().value, ["y", 2]], "mixed pair sources"],
]) {
  promise_test(async () => {
    assert_value_equals(exports.roundtripStringU32(input), new Map([["x", 1], ["y", 2]]));
  }, `map: ${description} is accepted`);
}

promise_test(async () => {
  const result = exports.roundtripStringU32([["a", 1], ["b", 2], ["a", 3]]);
  assert_value_equals(result, new Map([["a", 3], ["b", 2]]));
  assert_array_equals([...result.keys()], ["a", "b"], "first position is kept");
}, "map: duplicate keys keep the last value in the first position");

promise_test(async () => {
  const result = exports.roundtripS64String([[1n, "a"], [-1n, "b"], ["1", "c"]]);
  assert_value_equals(result, new Map([[1n, "c"], [-1n, "b"]]));
}, "map<s64, _>: keys are compared after conversion");

promise_test(async () => {
  const input = new Map([[0n, "zero"], [2n ** 63n - 1n, "max"], [-(2n ** 63n), "min"]]);
  assert_value_equals(exports.roundtripS64String(input), input);
}, "map<s64, _>: BigInt keys roundtrip");

promise_test(async () => {
  const result = exports.roundtripU32String(new Map([[1, "one"], ["2", "two"], [3.7, "three"]]));
  assert_value_equals(result, new Map([[1, "one"], [2, "two"], [3, "three"]]));
  assert_value_equals(exports.roundtripStringU32([["a", "1"], [2, 3]]), new Map([["a", 1], ["2", 3]]));
}, "map: keys and values are converted to their component types");

promise_test(async () => {
  const result = exports.roundtripStringU32({ b: 1, a: 2, 10: 3, [Symbol("s")]: 4 });
  assert_value_equals(result, new Map([["10", 3], ["b", 1], ["a", 2]]));
}, "map<string, _>: a non-iterable object yields its own enumerable string keys in property order");

promise_test(async () => {
  const input = Object.create({ inherited: 1 });
  input.own = 2;
  Object.defineProperty(input, "hidden", { value: 3, enumerable: false });
  assert_value_equals(exports.roundtripStringU32(input), new Map([["own", 2]]));
}, "map<string, _>: inherited and non-enumerable properties are ignored");

promise_test(async () => {
  const log = [];
  const input = new Proxy({ a: 1 }, {
    get(t, key, r) { log.push(`get ${String(key)}`); return Reflect.get(t, key, r); },
    ownKeys(t) { log.push("ownKeys"); return Reflect.ownKeys(t); },
    getOwnPropertyDescriptor(t, key) { log.push(`gopd ${String(key)}`); return Reflect.getOwnPropertyDescriptor(t, key); },
  });
  assert_value_equals(exports.roundtripStringU32(input), new Map([["a", 1]]));
  assert_equals(log[0], "get Symbol(Symbol.iterator)", "the iterator is looked up first");
  assert_true(log.includes("ownKeys"), "own keys are enumerated");
  assert_true(log.includes("get a"), "values are read with Get");
}, "map<string, _>: the iterable check precedes property enumeration");

for (const [name, description] of [["roundtripU32String", "map<u32, _>"], ["roundtripS64String", "map<s64, _>"]]) {
  promise_test(async () => {
    received = "unchanged";
    assert_throws_js(TypeError, () => exports[name]({ 1: "a" }));
    assert_equals(received, "unchanged", "echo must not be called");
  }, `${description}: a non-iterable object throws TypeError`);
}

// Entries are converted after the list is allocated in the guest, so a failing
// entry locks down the instance, and those tests use a fresh one.
for (const [input, description] of [
  [[["a"]], "a pair with one element"],
  [[["a", 1, 2]], "a pair with three elements"],
  [["a"], "a string element instead of a pair"],
  [[null], "a null element"],
  [[["a", 1n]], "a value that fails conversion"],
]) {
  promise_test(async () => {
    received = "unchanged";
    const e = await componentExports("values/map", imports);
    assert_throws_js(TypeError, () => e.roundtripStringU32(input));
    assert_equals(received, "unchanged", "echo must not be called");
    assert_traps(() => e.roundtripStringU32([]));
  }, `map: ${description} throws TypeError and locks down the instance`);
}

for (const [input, description] of [
  [null, "null"],
  [undefined, "undefined"],
  ["ab", "a string"],
  [1, "a number"],
]) {
  promise_test(async () => {
    received = "unchanged";
    assert_throws_js(TypeError, () => exports.roundtripStringU32(input));
    assert_equals(received, "unchanged", "echo must not be called");
  }, `map: ${description} throws TypeError`);
}

promise_test(async () => {
  exports.roundtripStringU32([["k", 1]]);
  assert_equals(Object.getPrototypeOf(received), Map.prototype);
  assert_true(received instanceof Map);
}, "map: the import receives a Map");

promise_test(async () => {
  const { roundtripStringU32 } = await componentExports("values/map", {
    echoStringU32: () => ({ z: 26 }), echoS64String: x => x, echoU32String: x => x,
  });
  assert_value_equals(roundtripStringU32(new Map()), new Map([["z", 26]]));
}, "map: an import may return a plain object for string keys");

promise_test(async () => {
  const { roundtripStringU32 } = await componentExports("values/map", {
    echoStringU32: () => 5, echoS64String: x => x, echoU32String: x => x,
  });
  assert_traps(() => roundtripStringU32(new Map()));
}, "map: an import returning a non-object traps");
