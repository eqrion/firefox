// META: global=window,dedicatedworker,jsshell
// META: script=/wasm/component/jsapi/harness/component-helpers.js
// META: script=/wasm/component/jsapi/harness/assertions.js

// WebAssembly.validate dispatches on the `layer` field of the binary header.

promise_test(async () => {
  assert_true(WebAssembly.validate(await componentBytes("constructor/no-imports")));
}, "validate: a valid component is valid");

promise_test(async () => {
  assert_true(WebAssembly.validate(await componentBytes("constructor/echo")));
}, "validate: a valid component with imports is valid");

test(() => {
  assert_true(WebAssembly.validate(emptyModuleBytes));
}, "validate: a core module is still valid");

test(() => {
  assert_true(WebAssembly.validate(emptyComponentBytes));
}, "validate: an empty component is valid");

test(() => {
  assert_false(WebAssembly.validate(new Uint8Array([...emptyComponentBytes, 0xff, 0xff, 0xff])));
}, "validate: garbage after the component header is invalid");

test(() => {
  assert_false(WebAssembly.validate(new Uint8Array([0x00, 0x61, 0x73, 0x6d, 0x0d, 0x00, 0x02, 0x00])));
}, "validate: an unknown layer is invalid");

test(() => {
  assert_false(WebAssembly.validate(new Uint8Array([0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x01, 0x00])));
}, "validate: an unknown component version is invalid");

test(() => {
  assert_false(WebAssembly.validate(new Uint8Array()));
}, "validate: empty bytes are invalid");

test(() => {
  assert_false(WebAssembly.validate(emptyComponentBytes.subarray(0, 4)));
}, "validate: a truncated header is invalid");

promise_test(async () => {
  const bytes = await componentBytesCopy("constructor/no-imports");
  bytes[bytes.length - 1] ^= 0xff;
  assert_false(WebAssembly.validate(bytes));
}, "validate: a corrupted component is invalid");

test(() => {
  for (const argument of [undefined, null, true, "", Symbol(), 1, {}, [], WebAssembly.Module]) {
    assert_throws_js(TypeError, () => WebAssembly.validate(argument),
                     `validate(${format_value(argument)})`);
  }
}, "validate: non-BufferSource arguments throw TypeError");

promise_test(async () => {
  const bytes = await componentBytes("constructor/no-imports");
  const buffer = new ArrayBuffer(bytes.length, { maxByteLength: bytes.length * 2 });
  new Uint8Array(buffer).set(bytes);
  assert_true(WebAssembly.validate(buffer));
  assert_true(WebAssembly.validate(new Uint8Array(buffer)));
}, "validate: resizable ArrayBuffer is accepted");

promise_test(async () => {
  if (typeof SharedArrayBuffer === "undefined") {
    return;
  }
  const bytes = await componentBytes("constructor/no-imports");
  const buffer = new SharedArrayBuffer(bytes.length);
  new Uint8Array(buffer).set(bytes);
  assert_true(WebAssembly.validate(buffer));
  assert_true(WebAssembly.validate(new Uint8Array(buffer)));
}, "validate: SharedArrayBuffer is accepted");

promise_test(async () => {
  const bytes = await componentBytes("constructor/no-imports");
  for (const view of [new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength),
                      new Int8Array(bytes.buffer, bytes.byteOffset, bytes.byteLength)]) {
    assert_true(WebAssembly.validate(view), format_value(view));
  }
}, "validate: any ArrayBufferView is accepted");

promise_test(async () => {
  const bytes = await componentBytes("constructor/no-imports");
  assert_true(WebAssembly.validate(bytes, {}));
  assert_true(WebAssembly.validate(bytes, { builtins: [] }));
  assert_true(WebAssembly.validate(bytes, { builtins: ["js/global"] }));
  assert_true(WebAssembly.validate(bytes, { builtins: ["bogus"] }));
  assert_throws_js(TypeError, () => WebAssembly.validate(bytes, { builtins: 1 }));
  assert_throws_js(TypeError, () => WebAssembly.validate(bytes, { builtins: [Symbol()] }));
  assert_throws_js(TypeError, () => WebAssembly.validate(bytes, 1));
}, "validate: compile options are parsed");
