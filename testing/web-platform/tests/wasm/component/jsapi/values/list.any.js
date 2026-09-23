// META: global=window,dedicatedworker,jsshell
// META: script=/wasm/component/jsapi/harness/component-helpers.js
// META: script=/wasm/component/jsapi/harness/assertions.js

// list<T>: any iterable object converts. The output is a typed array for a
// list of scalars like list<u32>, and a dense Array otherwise.

let exports;
const received = {};
const imports = {
  echoU32s: x => { received.u32s = x; return x; },
  echoStrings: x => { received.strings = x; return x; },
  echoNested: x => { received.nested = x; return x; },
};
promise_setup(async () => {
  exports = await componentExports("values/list", imports);
});

function assert_u32_array(actual, expected, description) {
  assert_equals(Object.getPrototypeOf(actual), Uint32Array.prototype, `${description}: prototype`);
  assert_value_equals(actual, new Uint32Array(expected), description);
}

function assert_dense_array(actual, expected, description) {
  assert_true(Array.isArray(actual), `${description}: Array.isArray`);
  assert_equals(Object.getPrototypeOf(actual), Array.prototype, `${description}: prototype`);
  assert_equals(actual.length, expected.length, `${description}: length`);
  assert_array_equals(Object.keys(actual), expected.map((_, i) => `${i}`), `${description}: dense`);
  assert_value_equals(actual, expected, description);
}

for (const [input, description] of [
  [[], "empty"],
  [[0], "one element"],
  [[1, 2, 3, 0xffffffff], "several elements"],
  [Array.from({ length: 10000 }, (_, i) => i), "10000 elements"],
]) {
  promise_test(async () => {
    const result = exports.roundtripU32s(input);
    assert_u32_array(received.u32s, input, "value seen by echo");
    assert_u32_array(result, input, "result");
    assert_not_equals(result.buffer, received.u32s.buffer, "result has a fresh buffer");
  }, `list<u32>: an Array of ${description} roundtrips`);
}

promise_test(async () => {
  const input = ["", "a", "héllo", "😀", "x".repeat(1000)];
  assert_dense_array(exports.roundtripStrings(input), input, "result");
  assert_dense_array(received.strings, input, "value seen by echo");
}, "list<string>: roundtrips");

promise_test(async () => {
  const input = [[], [1], [2, 3], []];
  const expected = input.map(inner => new Uint32Array(inner));
  assert_dense_array(exports.roundtripNested(input), expected, "result");
  assert_dense_array(received.nested, expected, "value seen by echo");
}, "list<list<u32>>: roundtrips as an Array of Uint32Arrays");

promise_test(async () => {
  const input = new Uint32Array([1, 2, 3]);
  const result = exports.roundtripU32s(input);
  assert_u32_array(result, [1, 2, 3], "result");
  assert_not_equals(result, input, "result is a fresh Uint32Array");
  assert_not_equals(result.buffer, input.buffer, "result has a fresh buffer");
}, "list<u32>: a Uint32Array is copied");

promise_test(async () => {
  const input = new Uint32Array([1, 2, 3]);
  input[Symbol.iterator] = () => { throw new Error("must not iterate"); };
  assert_u32_array(exports.roundtripU32s(input), [1, 2, 3], "result");
  const view = new Uint32Array(new Uint32Array([5, 6, 7, 8]).buffer, 4, 2);
  assert_u32_array(exports.roundtripU32s(view), [6, 7], "a view copies only its elements");
}, "list<u32>: a Uint32Array is copied directly, not iterated");

promise_test(async () => {
  const buffer = new ArrayBuffer(8);
  const input = new Uint32Array(buffer);
  buffer.transfer();
  assert_throws_js(TypeError, () => exports.roundtripU32s(input));
}, "list<u32>: a detached Uint32Array throws TypeError");

for (const [make, expected, description] of [
  [() => new Set([1, 2, 2, 3]), [1, 2, 3], "a Set"],
  [() => (function* () { yield 4; yield 5; })(), [4, 5], "a generator"],
  [() => ({ *[Symbol.iterator]() { yield 6; yield 7; } }), [6, 7], "a custom iterable"],
  [() => new Map([[1, 2]]).keys(), [1], "a Map iterator"],
  [() => new Int32Array([8, -1]), [8, 0xffffffff], "an Int32Array (elements wrap)"],
  [() => new Float32Array([1.5]), [1], "a Float32Array (elements truncate)"],
  [() => ["1", true, null, [2], undefined, -1, 2 ** 32 + 1], [1, 1, 0, 2, 0, 0xffffffff, 1],
   "an Array of coercible elements"],
  [() => [1, , 3], [1, 0, 3], "an Array with a hole"],
]) {
  promise_test(async () => {
    assert_u32_array(exports.roundtripU32s(make()), expected, "result");
  }, `list<u32>: ${description} is converted through the iterable path`);
}

promise_test(async () => {
  assert_dense_array(exports.roundtripStrings("abc"[Symbol.iterator]()), ["a", "b", "c"], "result");
}, "list<string>: a string iterator is an iterable object and converts");

promise_test(async () => {
  assert_dense_array(exports.roundtripStrings([1, null, undefined, true]), ["1", "null", "undefined", "true"],
                     "result");
}, "list<string>: elements go through ToString");

promise_test(async () => {
  assert_dense_array(exports.roundtripStrings([, "a", , ]), ["undefined", "a", "undefined"], "result");
}, "list<string>: holes are undefined, which stringifies");

// Elements are converted after the list is allocated in the guest, so a
// failing element locks down the instance, and those tests use a fresh one.
async function freshExports() {
  return componentExports("values/list", imports);
}

promise_test(async () => {
  const e = await freshExports();
  assert_throws_js(TypeError, () => e.roundtripU32s([1n]));
  assert_traps(() => e.roundtripU32s([]));
}, "list<u32>: a failing element locks down the instance, since the list was allocated");

promise_test(async () => {
  const e = await freshExports();
  assert_throws_js(TypeError, () => e.roundtripU32s(5));
  assert_u32_array(e.roundtripU32s([1]), [1], "result");
}, "list<u32>: a non-iterable value does not lock down the instance");

for (const [make, description] of [
  [() => "abc", "a string (not an Object, even though iterable)"],
  [() => 42, "a Number"],
  [() => null, "null"],
  [() => undefined, "undefined"],
  [() => true, "a boolean"],
  [() => Symbol.iterator, "a Symbol"],
  [() => ({}), "a plain object"],
  [() => ({ length: 1, 0: 1 }), "an array-like that is not iterable"],
  [() => ({ [Symbol.iterator]: 1 }), "an object whose Symbol.iterator is not callable"],
  [() => ({ [Symbol.iterator]() { return 1; } }), "an object whose iterator is not an object"],
  [() => ({ [Symbol.iterator]() { return { next: 1 }; } }), "an iterator whose next is not callable"],
  [() => ({ [Symbol.iterator]() { return { next() { return 1; } }; } }),
   "an iterator whose result is not an object"],
  [() => [1n], "an Array with a BigInt element"],
  [() => [Symbol()], "an Array with a Symbol element"],
  [() => new BigUint64Array([1n]), "a BigUint64Array (elements are BigInts)"],
]) {
  promise_test(async () => {
    received.u32s = "unchanged";
    const e = await freshExports();
    assert_throws_js(TypeError, () => e.roundtripU32s(make()));
    assert_equals(received.u32s, "unchanged", "echo must not be called");
  }, `list<u32>: ${description} throws TypeError`);
}

for (const [input, description] of [
  [[[1], 2], "a non-iterable inner list"],
  [[[1], [1n]], "a BigInt inner element"],
  [["ab"], "a string inner list"],
]) {
  promise_test(async () => {
    const e = await freshExports();
    assert_throws_js(TypeError, () => e.roundtripNested(input));
  }, `list<list<u32>>: ${description} throws TypeError`);
}

promise_test(async () => {
  const visited = [];
  const iterable = {
    *[Symbol.iterator]() {
      for (const v of [1, 1n, 2]) {
        visited.push(v);
        yield v;
      }
    },
  };
  const e = await freshExports();
  assert_throws_js(TypeError, () => e.roundtripU32s(iterable));
  assert_array_equals(visited, [1, 1n, 2], "iteration finishes before conversion");
}, "list<u32>: the iterable is consumed before any element is converted");

promise_test(async () => {
  const error = new Error("from iterator");
  const iterable = { [Symbol.iterator]() { return { next() { throw error; } }; } };
  assert_throws_exactly(error, () => exports.roundtripU32s(iterable));
}, "list<u32>: an exception from next() propagates as-is");

promise_test(async () => {
  const error = new Error("from getter");
  const iterable = { get [Symbol.iterator]() { throw error; } };
  assert_throws_exactly(error, () => exports.roundtripU32s(iterable));
}, "list<u32>: an exception from the Symbol.iterator getter propagates as-is");

promise_test(async () => {
  let calls = 0;
  const iterable = { get [Symbol.iterator]() { calls++; return function* () { yield 1; }; } };
  exports.roundtripU32s(iterable);
  assert_equals(calls, 1);
}, "list<u32>: Symbol.iterator is read exactly once");

promise_test(async () => {
  const e = await componentExports("values/list", { ...imports, echoU32s: () => new Set([1, 2]) });
  assert_u32_array(e.roundtripU32s([]), [1, 2], "result");
}, "list<u32>: an import returning a Set is converted");

for (const [value, description] of [
  [5, "a Number"],
  ["ab", "a string"],
  [undefined, "undefined"],
  [{}, "a non-iterable object"],
  [[1n], "an Array with a BigInt element"],
]) {
  promise_test(async () => {
    const e = await componentExports("values/list", { ...imports, echoU32s: () => value });
    assert_traps(() => e.roundtripU32s([]));
  }, `list<u32>: an import returning ${description} traps`);
}

promise_test(async () => {
  let closed = 0;
  const iterable = {
    [Symbol.iterator]() {
      const values = [1, 1n, 2];
      return {
        next() { return values.length ? { value: values.shift(), done: false } : { done: true }; },
        return() { closed++; return {}; },
      };
    },
  };
  const e = await freshExports();
  assert_throws_js(TypeError, () => e.roundtripU32s(iterable));
  assert_equals(closed, 0, "the exhausted iterator is not closed");
}, "list<u32>: a failing element does not call return()");

promise_test(async () => {
  let closed = 0;
  const iterable = {
    [Symbol.iterator]() {
      const values = [1, 2];
      return {
        next() { return values.length ? { value: values.shift(), done: false } : { done: true }; },
        return() { closed++; return {}; },
      };
    },
  };
  assert_u32_array(exports.roundtripU32s(iterable), [1, 2], "result");
  assert_equals(closed, 0, "an exhausted iterator is not closed again");
}, "list<u32>: a completed iteration does not call return()");
