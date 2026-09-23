// META: global=window,dedicatedworker,jsshell
// META: script=/wasm/component/jsapi/harness/component-helpers.js
// META: script=/wasm/component/jsapi/harness/assertions.js

// list<u8>, like every list of scalars, is special-cased: a Uint8Array is
// copied directly, everything else goes through the generic iterable path, and
// the output is always a fresh Uint8Array.

let exports;
let received;
const spied = [];
promise_setup(async () => {
  const echoBytes = x => { received = x; return x; };
  const recv = x => { spied.push(x); };
  exports = await componentExports("values/list-u8", { echoBytes, recv });
});

function assert_fresh_bytes(actual, expectedBytes, description) {
  assert_equals(Object.getPrototypeOf(actual), Uint8Array.prototype, `${description}: prototype`);
  assert_equals(actual.byteOffset, 0, `${description}: byteOffset`);
  assert_equals(actual.length, expectedBytes.length, `${description}: length`);
  assert_equals(actual.buffer.byteLength, expectedBytes.length, `${description}: buffer is exactly sized`);
  assert_false(actual.buffer.resizable, `${description}: buffer is not resizable`);
  assert_array_equals(Array.from(actual), expectedBytes, `${description}: contents`);
}

for (const [bytes, description] of [
  [[], "empty"],
  [[0], "one zero byte"],
  [[0, 1, 127, 128, 254, 255], "the full byte range edges"],
  [Array.from({ length: 1 << 20 }, (_, i) => i & 0xff), "1 MiB"],
]) {
  promise_test(async () => {
    const input = new Uint8Array(bytes);
    const result = exports.roundtripBytes(input);
    assert_fresh_bytes(received, bytes, "value seen by echo");
    assert_fresh_bytes(result, bytes, "result");
    assert_not_equals(result, input, "result is a different object");
    assert_not_equals(result.buffer, input.buffer, "result has its own buffer");
    assert_not_equals(received, input, "echo sees a different object");
  }, `list<u8>: Uint8Array of ${description} roundtrips as a fresh Uint8Array`);
}

promise_test(async () => {
  const input = new Uint8Array([1, 2, 3]);
  const { roundtripBytes } = await componentExports("values/list-u8", {
    echoBytes(x) {
      input[0] = 99;
      x[1] = 42;
      return x;
    },
    recv() {},
  });
  const result = roundtripBytes(input);
  assert_array_equals(Array.from(result), [1, 42, 3], "echo's mutation of its copy is what gets lowered");
  assert_array_equals(Array.from(input), [99, 2, 3], "the caller's array is untouched by the conversion");
}, "list<u8>: the argument is copied before echo runs, and echo's return value is copied after");

promise_test(async () => {
  const buffer = new Uint8Array([9, 9, 1, 2, 3, 9]).buffer;
  const view = new Uint8Array(buffer, 2, 3);
  assert_fresh_bytes(exports.roundtripBytes(view), [1, 2, 3], "result");
}, "list<u8>: a Uint8Array view honours byteOffset and length");

promise_test(async () => {
  const buffer = new ArrayBuffer(8, { maxByteLength: 16 });
  const view = new Uint8Array(buffer);
  view.set([1, 2, 3, 4, 5, 6, 7, 8]);
  assert_fresh_bytes(exports.roundtripBytes(view), [1, 2, 3, 4, 5, 6, 7, 8], "result");
  buffer.resize(3);
  assert_fresh_bytes(exports.roundtripBytes(view), [1, 2, 3], "result after shrinking");
}, "list<u8>: a length-tracking view on a resizable buffer uses its current length");

promise_test(async () => {
  const buffer = new ArrayBuffer(8, { maxByteLength: 16 });
  const view = new Uint8Array(buffer, 4);
  buffer.resize(2);
  received = "unchanged";
  assert_throws_js(TypeError, () => exports.roundtripBytes(view));
  assert_equals(received, "unchanged", "echo must not be called");
}, "list<u8>: an out-of-bounds Uint8Array throws TypeError");

promise_test(async () => {
  const view = new Uint8Array([1, 2, 3]);
  view.buffer.transfer();
  received = "unchanged";
  assert_throws_js(TypeError, () => exports.roundtripBytes(view));
  assert_equals(received, "unchanged", "echo must not be called");
}, "list<u8>: a Uint8Array over a detached buffer throws TypeError");

promise_test(async () => {
  const shared = new Uint8Array(new SharedArrayBuffer(3));
  shared.set([7, 8, 9]);
  assert_fresh_bytes(exports.roundtripBytes(shared), [7, 8, 9], "result");
  assert_equals(Object.getPrototypeOf(received.buffer), ArrayBuffer.prototype, "echo sees a non-shared copy");
}, "list<u8>: a Uint8Array over a SharedArrayBuffer is copied");

for (const [make, bytes, description] of [
  [() => [1, 2, 3], [1, 2, 3], "an Array of Numbers"],
  [() => ["1", true, null], [1, 1, 0], "an Array of coercible values"],
  [() => new Uint8ClampedArray([1, 255]), [1, 255], "a Uint8ClampedArray"],
  [() => new Int8Array([1, -1]), [1, 255], "an Int8Array (elements wrap)"],
  [() => new Uint16Array([1, 256]), [1, 0], "a Uint16Array (elements wrap)"],
  [() => new Float64Array([1.9, 2.1]), [1, 2], "a Float64Array (elements truncate)"],
  [() => [256, -1, 1.5, NaN, undefined], [0, 255, 1, 0, 0], "an Array of values that wrap or are 0"],
  [() => new Set([3, 4]), [3, 4], "a Set"],
  [() => (function* () { yield 5; yield 6; })(), [5, 6], "a generator"],
  [() => ({ *[Symbol.iterator]() { yield 7; } }), [7], "a custom iterable"],
  [() => [], [], "an empty Array"],
]) {
  promise_test(async () => {
    assert_fresh_bytes(exports.roundtripBytes(make()), bytes, "result");
    assert_fresh_bytes(received, bytes, "value seen by echo");
  }, `list<u8>: ${description} is converted through the iterable path`);
}

for (const [make, description] of [
  [() => [1n], "an Array with a BigInt"],
  [() => [Symbol()], "an Array with a Symbol"],
  [() => new ArrayBuffer(4), "an ArrayBuffer"],
  [() => new DataView(new ArrayBuffer(4)), "a DataView"],
  [() => "abc", "a string"],
  [() => 42, "a Number"],
  [() => null, "null"],
  [() => undefined, "undefined"],
  [() => ({}), "a plain object"],
  [() => ({ length: 1, 0: 1 }), "an array-like that is not iterable"],
  [() => ({ [Symbol.iterator]: 1 }), "an object whose Symbol.iterator is not callable"],
  [() => ({ [Symbol.iterator]() { return 1; } }), "an object whose iterator is not an object"],
]) {
  // Elements are converted after the list is allocated in the guest, so a
  // failing element locks down the instance, and each case uses a fresh one.
  promise_test(async () => {
    received = "unchanged";
    const { roundtripBytes } = await componentExports("values/list-u8", {
      echoBytes: x => { received = x; return x; },
      recv() {},
    });
    assert_throws_js(TypeError, () => roundtripBytes(make()));
    assert_equals(received, "unchanged", "echo must not be called");
  }, `list<u8>: ${description} throws TypeError`);
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
  const { roundtripBytes } = await componentExports("values/list-u8", { echoBytes: x => x, recv() {} });
  assert_throws_js(TypeError, () => roundtripBytes(iterable));
  assert_array_equals(visited, [1, 1n, 2], "iteration finishes before conversion");
}, "list<u8>: the iterable is consumed before any element is converted");

promise_test(async () => {
  const { roundtripBytes } = await componentExports("values/list-u8", { echoBytes: () => [1, 2], recv() {} });
  assert_fresh_bytes(roundtripBytes(new Uint8Array()), [1, 2], "result");
}, "list<u8>: an import returning an Array is converted");

for (const [value, description] of [
  [[1n], "an Array with a BigInt"],
  [5, "a Number"],
  ["ab", "a string"],
  [undefined, "undefined"],
]) {
  promise_test(async () => {
    const { roundtripBytes } = await componentExports("values/list-u8", { echoBytes: () => value, recv() {} });
    assert_traps(() => roundtripBytes(new Uint8Array()));
  }, `list<u8>: an import returning ${description} traps`);
}

promise_test(async () => {
  spied.length = 0;
  exports.send();
  assert_equals(spied.length, 2);
  assert_fresh_bytes(spied[0], [0, 1, 127, 128, 254, 255], "first value");
  assert_fresh_bytes(spied[1], [], "second value");
  assert_not_equals(spied[0].buffer, spied[1].buffer, "each lift has its own buffer");
}, "list<u8>: guest-produced bytes lift to fresh Uint8Arrays");
