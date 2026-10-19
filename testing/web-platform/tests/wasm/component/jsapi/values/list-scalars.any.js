// META: global=window,dedicatedworker,jsshell
// META: script=/wasm/component/jsapi/harness/component-helpers.js
// META: script=/wasm/component/jsapi/harness/assertions.js

// A list of any scalar type is a typed array of the matching class. A typed
// array of that class is copied directly, and anything else goes through the
// iterable path like the typed array constructors do.

const types = {
  s8: { ctor: Int8Array, values: [-128, -1, 0, 127] },
  u8: { ctor: Uint8Array, values: [0, 1, 255] },
  s16: { ctor: Int16Array, values: [-32768, -1, 0, 32767] },
  u16: { ctor: Uint16Array, values: [0, 1, 65535] },
  s32: { ctor: Int32Array, values: [-(2 ** 31), -1, 0, 2 ** 31 - 1] },
  u32: { ctor: Uint32Array, values: [0, 1, 2 ** 32 - 1] },
  s64: { ctor: BigInt64Array, values: [-(2n ** 63n), -1n, 0n, 2n ** 63n - 1n] },
  u64: { ctor: BigUint64Array, values: [0n, 1n, 2n ** 64n - 1n] },
  f32: { ctor: Float32Array, values: [-0, 1.5, Infinity, -Infinity, NaN] },
  f64: { ctor: Float64Array, values: [-0, 0.1, Number.MAX_VALUE, -Infinity, NaN] },
};

function capitalize(type) {
  return type[0].toUpperCase() + type.slice(1);
}

const received = {};
const imports = { recvF32() {}, recvF64() {} };
for (const type of [...Object.keys(types), "f64x2"]) {
  imports[`echo${capitalize(type)}`] = x => { received[type] = x; return x; };
}

let exports;
promise_setup(async () => {
  exports = await componentExports("values/list-scalars", imports);
});

function assert_typed_array(actual, ctor, values, description) {
  assert_equals(Object.getPrototypeOf(actual), ctor.prototype, `${description}: prototype`);
  assert_equals(actual.byteOffset, 0, `${description}: byteOffset`);
  assert_equals(actual.buffer.byteLength, values.length * ctor.BYTES_PER_ELEMENT,
                `${description}: buffer is exactly sized`);
  assert_value_equals(actual, new ctor(values), description);
}

for (const [type, { ctor, values }] of Object.entries(types)) {
  const roundtrip = x => exports[`roundtrip${capitalize(type)}`](x);

  promise_test(async () => {
    const input = new ctor(values);
    input[Symbol.iterator] = () => { throw new Error("must not iterate"); };
    const result = roundtrip(input);
    assert_typed_array(result, ctor, values, "result");
    assert_typed_array(received[type], ctor, values, "value seen by echo");
    assert_not_equals(result.buffer, input.buffer, "result has its own buffer");
  }, `list<${type}>: a ${ctor.name} is copied directly and roundtrips as a fresh ${ctor.name}`);

  promise_test(async () => {
    assert_typed_array(roundtrip(values), ctor, values, "an Array");
    assert_typed_array(roundtrip(new Set(values)), ctor, [...new Set(values)], "a Set");
    assert_typed_array(roundtrip([]), ctor, [], "an empty Array");
  }, `list<${type}>: an iterable is converted element by element`);

  promise_test(async () => {
    const buffer = new ArrayBuffer(4 * ctor.BYTES_PER_ELEMENT);
    const input = new ctor(buffer, ctor.BYTES_PER_ELEMENT, 2);
    input.set(values.slice(0, 2));
    assert_typed_array(roundtrip(input), ctor, values.slice(0, 2), "result");
  }, `list<${type}>: a ${ctor.name} view copies only its elements`);

  promise_test(async () => {
    const input = new ctor(2);
    input.buffer.transfer();
    assert_throws_js(TypeError, () => roundtrip(input));
  }, `list<${type}>: a detached ${ctor.name} throws TypeError`);
}

promise_test(async () => {
  assert_typed_array(exports.roundtripS8([255, 128, -129, 1.9, "2", null]), Int8Array,
                     [-1, -128, 127, 1, 2, 0], "list<s8>");
  assert_typed_array(exports.roundtripU16(new Int32Array([-1, 65536])), Uint16Array, [65535, 0], "list<u16>");
  assert_typed_array(exports.roundtripS64([2n ** 63n, "5", true]), BigInt64Array, [-(2n ** 63n), 5n, 1n],
                     "list<s64>");
  assert_typed_array(exports.roundtripF32([0.1, "1e40"]), Float32Array, [Math.fround(0.1), Infinity],
                     "list<f32>");
}, "list of scalars: other values convert like the scalar type");

promise_test(async () => {
  const e = await componentExports("values/list-scalars", imports);
  assert_throws_js(TypeError, () => e.roundtripS64([1]), "a Number for s64");
  const f = await componentExports("values/list-scalars", imports);
  assert_throws_js(TypeError, () => f.roundtripU32([1n]), "a BigInt for u32");
  const g = await componentExports("values/list-scalars", imports);
  assert_throws_js(TypeError, () => g.roundtripS64(new Int32Array([1])), "an Int32Array for s64");
}, "list of scalars: elements that fail to convert throw TypeError");

promise_test(async () => {
  const sent = [];
  const { sendNans } = await componentExports("values/list-scalars", {
    ...imports, recvF32: x => sent.push(x), recvF64: x => sent.push(x),
  });
  sendNans();
  assert_typed_array(sent[0], Float32Array, [NaN, NaN], "f32");
  assert_array_equals(new Uint32Array(sent[0].buffer), [0x7fc00000, 0x7fc00000], "f32 NaN bits");
  assert_typed_array(sent[1], Float64Array, [NaN, NaN], "f64");
  assert_array_equals(new BigUint64Array(sent[1].buffer), [0x7ff8000000000000n, 0x7ff8000000000000n],
                      "f64 NaN bits");
}, "list of floats: NaNs loaded from memory are canonicalized");

promise_test(async () => {
  const input = new Float64Array([1.5, NaN]);
  input[Symbol.iterator] = () => { throw new Error("must not iterate"); };
  const result = exports.roundtripF64x2(input);
  assert_typed_array(result, Float64Array, [1.5, NaN], "result");
  assert_typed_array(received.f64x2, Float64Array, [1.5, NaN], "value seen by echo");
  assert_typed_array(exports.roundtripF64x2(["2", 3]), Float64Array, [2, 3], "an Array");
  assert_throws_js(TypeError, () => exports.roundtripF64x2(new Float64Array(3)), "wrong length");
}, "list<f64, 2>: a Float64Array of two elements");
