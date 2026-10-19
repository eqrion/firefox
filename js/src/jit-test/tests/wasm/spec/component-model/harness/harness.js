"use strict";

/* Copyright 2021 Mozilla Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

if (!wasmIsSupported()) {
  quit();
}

function partialOobWriteMayWritePartialData() {
  let arm_native = getBuildConfiguration("arm") && !getBuildConfiguration("arm-simulator");
  let arm64_native = getBuildConfiguration("arm64") && !getBuildConfiguration("arm64-simulator");
  let riscv64_native = getBuildConfiguration("riscv64") && !getBuildConfiguration("riscv64-simulator");
  return arm_native || arm64_native || riscv64_native;
}

function bytes(type, bytes) {
  var typedBuffer = new Uint8Array(bytes);
  return wasmGlobalFromArrayBuffer(type, typedBuffer.buffer);
}
function value(type, value) {
  return new WebAssembly.Global({
    value: type,
    mutable: false,
  }, value);
}

function i8x16(elements) {
  let typedBuffer = new Uint8Array(elements);
  return wasmGlobalFromArrayBuffer("v128", typedBuffer.buffer);
}
function i16x8(elements) {
  let typedBuffer = new Uint16Array(elements);
  return wasmGlobalFromArrayBuffer("v128", typedBuffer.buffer);
}
function i32x4(elements) {
  let typedBuffer = new Uint32Array(elements);
  return wasmGlobalFromArrayBuffer("v128", typedBuffer.buffer);
}
function i64x2(elements) {
  let typedBuffer = new BigUint64Array(elements);
  return wasmGlobalFromArrayBuffer("v128", typedBuffer.buffer);
}
function f32x4(elements) {
  let typedBuffer = new Float32Array(elements);
  return wasmGlobalFromArrayBuffer("v128", typedBuffer.buffer);
}
function f64x2(elements) {
  let typedBuffer = new Float64Array(elements);
  return wasmGlobalFromArrayBuffer("v128", typedBuffer.buffer);
}

function either(...arr) {
  return new EitherVariants(arr);
}

class F32x4Pattern {
  constructor(x, y, z, w) {
    this.x = x;
    this.y = y;
    this.z = z;
    this.w = w;
  }
}

class F64x2Pattern {
  constructor(x, y) {
    this.x = x;
    this.y = y;
  }
}

class RefWithType {
  constructor(type) {
    this.type = type;
  }

  formatExpected() {
    return `RefWithType(${this.type})`;
  }

  test(refGlobal) {
    try {
      new WebAssembly.Global({value: this.type}, refGlobal.value);
      return true;
    } catch (err) {
      assertEq(err instanceof TypeError, true, `wrong type of error when creating global: ${err}`);
      assertEq(!!err.message.match(/can only pass/), true, `wrong type of error when creating global: ${err}`);
      return false;
    }
  }
}

// ref.extern values created by spec tests will be JS objects of the form
// { [externsym]: <number> }. Other externref values are possible to observe
// if extern.convert_any is used.
let externsym = Symbol("externref");
function externref(s) {
  return { [externsym]: s };
}

class ExternRefResult {
  constructor(n) {
    this.n = n;
  }

  formatExpected() {
    return `ref.extern ${this.n}`;
  }

  test(global) {
    // the global's value can either be an externref or just a plain old JS number
    let result = global.value;
    if (typeof global.value === "object" && externsym in global.value) {
      result = global.value[externsym];
    }
    return result === this.n;
  }
}

// ref.host values created by spectests will be whatever the JS API does to
// convert the given value to anyref. It should implicitly be like any.convert_extern.
function hostref(v) {
  const { internalizeNum } = new WebAssembly.Instance(
    new WebAssembly.Module(wasmTextToBinary(`(module
      (func (import "test" "coerce") (param i32) (result anyref))
      (func (export "internalizeNum") (param i32) (result anyref)
        (call 0 (local.get 0))
      )
    )`)),
    { "test": { "coerce": x => x } },
  ).exports;
  return internalizeNum(v);
}

class HostRefResult {
  constructor(n) {
    this.n = n;
  }

  formatExpected() {
    return `ref.host ${this.n}`;
  }

  test(externrefGlobal) {
    assertEq(externsym in externrefGlobal.value, true, `HostRefResult only works with externref inputs`);
    return externrefGlobal.value[externsym] === this.n;
  }
}

// https://github.com/WebAssembly/spec/blob/main/interpreter/README.md#spectest-host-module
let linkage = {
  "spectest": {
    global_i32: 666,
    global_i64: 666n,
    global_f32: 666.6,
    global_f64: 666.6,

    table: new WebAssembly.Table({
      initial: 10,
      maximum: 20,
      element: "anyfunc",
    }),
    table64: new WebAssembly.Table({
      address: "i64",
      initial: 10n,
      maximum: 20n,
      element: "anyfunc",
    }),

    memory: new WebAssembly.Memory({ initial: 1, maximum: 2 }),

    print: console.log.bind(console),
    print_i32: console.log.bind(console),
    print_i64: console.log.bind(console),
    print_f32: console.log.bind(console),
    print_f64: console.log.bind(console),
    print_i32_f32: console.log.bind(console),
    print_f64_f64: console.log.bind(console),
  },
};

function module(source) {
  let bytecode = wasmTextToBinary(source);
  let module = new WebAssembly.Module(bytecode);
  return module;
}

function instantiate(source) {
  let bytecode = wasmTextToBinary(source);
  let module = new WebAssembly.Module(bytecode);
  let instance = new WebAssembly.Instance(module, linkage);
  return instance.exports;
}

function instantiateFromModule(module) {
  let instance = new WebAssembly.Instance(module, linkage);
  return instance.exports;
}

function register(instance, name) {
  linkage[name] = instance;
}

function invoke(instance, field, params) {
  let func = instance[field];
  assertEq(func instanceof Function, true, "expected a function");
  return wasmLosslessInvoke(func, ...params);
}

function get(instance, field) {
  let global = instance[field];
  assertEq(
    global instanceof WebAssembly.Global,
    true,
    "expected a WebAssembly.Global",
  );
  return global;
}

function assert_trap(thunk, message) {
  try {
    thunk();
    throw new Error(`got no error`);
  } catch (err) {
    if (err instanceof WebAssembly.RuntimeError) {
      return;
    }
    err.message = `expected trap (${message}): ${err.message}`;
    throw err;
  }
}

function assert_suspension(thunk, message) {
  // TODO: basically a duplicate of assert_trap, not sure what this actually
  // is supposed to be.
  assert_trap(thunk, message);
}

let StackOverflow;
try {
  (function f() {
    1 + f();
  })();
} catch (e) {
  StackOverflow = e.constructor;
}
function assert_exhaustion(thunk, message) {
  try {
    thunk();
    throw new Error(`got no error`);
  } catch (err) {
    if (err instanceof StackOverflow) {
      return;
    }
    err.message = `expected exhaustion (${message}): ${err.message}`;
    throw err;
  }
}

function assert_invalid(thunk, message) {
  try {
    thunk();
    throw new Error(`got no error`);
  } catch (err) {
    if (err instanceof WebAssembly.LinkError || err instanceof WebAssembly.CompileError) {
      return;
    }
    err.message = `expected invalid module (${message}): ${err.message}`;
    throw err;
  }
}

function assert_unlinkable(thunk, message) {
  try {
    thunk();
    throw new Error(`got no error`);
  } catch (err) {
    // Allow type errors only when they pertain to imports
    if (err instanceof TypeError && err.message.match(/import object field.*is not a/)) {
      return;
    }

    // Allow link errors and compile errors generally (we do not really care
    // about the specifics)
    if (err instanceof WebAssembly.LinkError || err instanceof WebAssembly.CompileError) {
      return;
    }

    err.message = `expected an unlinkable module (${message}): ${err.message}`;
    throw err;
  }
}

function assert_malformed(thunk, message) {
  try {
    thunk();
    throw new Error(`got no error`);
  } catch (err) {
    if (
      err instanceof SyntaxError ||
      err instanceof WebAssembly.CompileError
    ) {
      return;
    }
    err.message = `expected a malformed module (${message}): ${err.message}`;
    throw err;
  }
}

function assert_exception(thunk) {
  let thrown = false;
  try {
    thunk();
  } catch (err) {
    thrown = true;
  }
  assertEq(thrown, true, "expected an exception to be thrown");
}

function assert_return(thunk, expected) {
  let results = thunk();

  if (results === undefined) {
    results = [];
  } else if (!Array.isArray(results)) {
    results = [results];
  }
  if (!Array.isArray(expected)) {
    expected = [expected];
  }

  if (!compareResults(results, expected)) {
    let got = results.map((x) => formatResult(x)).join(", ");
    let wanted = expected.map((x) => formatExpected(x)).join(", ");
    assertEq(
      `[${got}]`,
      `[${wanted}]`,
    );
    assertEq(true, false, `${got} !== ${wanted}`);
  }
}

function formatResult(result) {
  if (typeof (result) === "object") {
    return wasmGlobalToString(result);
  } else {
    return `${result}`;
  }
}

function formatExpected(expected) {
  if (
    expected === `canonical_nan` ||
    expected === `arithmetic_nan`
  ) {
    return expected;
  } else if (expected instanceof F32x4Pattern) {
    return `f32x4(${formatExpected(expected.x)}, ${
      formatExpected(expected.y)
    }, ${formatExpected(expected.z)}, ${formatExpected(expected.w)})`;
  } else if (expected instanceof F64x2Pattern) {
    return `f64x2(${formatExpected(expected.x)}, ${
      formatExpected(expected.y)
    })`;
  } else if (expected instanceof EitherVariants) {
    return expected.formatExpected();
  } else if (expected instanceof RefWithType) {
    return expected.formatExpected();
  } else if (expected instanceof ExternRefResult) {
    return expected.formatExpected();
  } else if (expected instanceof HostRefResult) {
    return expected.formatExpected();
  } else if (typeof (expected) === "object") {
    return wasmGlobalToString(expected);
  } else {
    throw new Error("unknown expected result");
  }
}

class EitherVariants {
  constructor(arr) {
    this.arr = arr;
  }
  matches(v) {
    return this.arr.some((e) => compareResult(v, e));
  }
  formatExpected() {
    return `either(${this.arr.map(formatExpected).join(", ")})`;
  }
}

function compareResults(results, expected) {
  if (results.length !== expected.length) {
    return false;
  }
  for (let i in results) {
    if (expected[i] instanceof EitherVariants) {
      return expected[i].matches(results[i]);
    }
    if (!compareResult(results[i], expected[i])) {
      return false;
    }
  }
  return true;
}

function compareResult(result, expected) {
  if (
    expected === `canonical_nan` ||
    expected === `arithmetic_nan`
  ) {
    return wasmGlobalIsNaN(result, expected);
  } else if (expected === null) {
    return result.value === null;
  } else if (expected instanceof F32x4Pattern) {
    return compareResult(
      wasmGlobalExtractLane(result, "f32x4", 0),
      expected.x,
    ) &&
      compareResult(wasmGlobalExtractLane(result, "f32x4", 1), expected.y) &&
      compareResult(wasmGlobalExtractLane(result, "f32x4", 2), expected.z) &&
      compareResult(wasmGlobalExtractLane(result, "f32x4", 3), expected.w);
  } else if (expected instanceof F64x2Pattern) {
    return compareResult(
      wasmGlobalExtractLane(result, "f64x2", 0),
      expected.x,
    ) &&
      compareResult(wasmGlobalExtractLane(result, "f64x2", 1), expected.y);
  } else if (expected instanceof RefWithType) {
    return expected.test(result);
  } else if (expected instanceof ExternRefResult) {
    return expected.test(result);
  } else if (expected instanceof HostRefResult) {
    return expected.test(result);
  } else if (typeof (expected) === "object") {
    return wasmGlobalsEqual(result, expected);
  } else {
    throw new Error("unknown expected result");
  }
}

class Thread {
  LOC_STATE = 0;
  LOC_DID_ERROR = 1;

  STATE_WORKER_READY = 0x60; // "GO"
  STATE_SENDING_VALUE = 0xF00D; // feed me values
  STATE_GOT_VALUE = 0x600DF00D; // mm delicious values
  STATE_RUN_CODE = 0xC0DE;
  STATE_DONE = 0xDEAD;

  constructor(sharedModule, sharedModuleName, code) {
    this._coord = new Int32Array(new SharedArrayBuffer(4*2));

    setSharedObject(this._coord.buffer);
    evalInWorker(`
      const _coord = new Int32Array(getSharedObject());

      ${readRelativeToScript("harness.js")}

      function setState(state) {
        Atomics.store(_coord, ${this.LOC_STATE}, state);
      }
      function waitForState(expected) {
        while (Atomics.load(_coord, ${this.LOC_STATE}) !== expected) {}
      }
      function receive() {
        waitForState(${this.STATE_SENDING_VALUE});
        const x = getSharedObject();
        setState(${this.STATE_GOT_VALUE});
        return x;
      }

      // Tell main thread we are ready
      setState(${this.STATE_WORKER_READY});

      // Get shared module's exports from main thread. (We do this one at a
      // time for reasons explained below.)
      const ${sharedModuleName} = {};
      ${Object.keys(sharedModule ?? {}).map(name =>
        `${sharedModuleName}["${name}"] = receive();`
      )}
      waitForState(${this.STATE_RUN_CODE});

      try {
        ${code}
      } catch (e) {
        Atomics.store(_coord, ${this.LOC_DID_ERROR}, 1);
        throw e;
      } finally {
        setState(${this.STATE_DONE});
      }
    `);

    // Wait for worker to spawn
    this.waitForState(this.STATE_WORKER_READY);

    // Send shared module exports to worker. We send values one at a time
    // because setGlobalObject can only take very specific objects, like wasm
    // memories, not generic objects like the whole exports object.
    for (const exportedValue of Object.values(sharedModule ?? {})) {
      this.send(exportedValue);
    }

    // Give the worker the all-clear to execute its workload
    this.setState(this.STATE_RUN_CODE);
  }

  setState(state) {
    Atomics.store(this._coord, this.LOC_STATE, state);
  }

  waitForState(expected) {
    while (Atomics.load(this._coord, this.LOC_STATE) !== expected) {}
  }

  send(val) {
    setSharedObject(val);
    this.setState(this.STATE_SENDING_VALUE);
    this.waitForState(this.STATE_GOT_VALUE);
  }

  wait() {
    this.waitForState(this.STATE_DONE);
    if (Atomics.load(this._coord, this.LOC_DID_ERROR)) {
      throw new Error("Error in worker code. Note that line numbers will not be helpful because of how the harness is loaded.");
    }
  }
}

// Component model support

function component(source) {
  let bytecode = wasmTextToBinary(source);
  return new WebAssembly.Component(bytecode);
}

function instantiateComponent(source) {
  return instantiateFromComponent(component(source));
}

// TODO: Pass registered instances as imports.
function instantiateFromComponent(component) {
  return new WebAssembly.ComponentInstance(component);
}

// The JS-API's CamelCase and PascalCase of a label.
function componentLabelToJS(label, pascal) {
  return label.split("-").map((fragment, i) => {
    if (fragment === fragment.toUpperCase()) {
      return fragment;
    }
    if (i === 0 && !pascal) {
      return fragment;
    }
    return fragment[0].toUpperCase() + fragment.slice(1);
  }).join("");
}

// The JS-API's JSName of a function export.
function componentJSName(name) {
  return name.includes(":") ? name : componentLabelToJS(name, false);
}

// A `result` value, which is unwrapped in return position and is otherwise a
// variant.
class ComponentResult {
  constructor(isOk, value) {
    this.isOk = isOk;
    this.value = value;
  }
}

// A `flags` value, as the set of labels that are set.
class ComponentFlags {
  constructor(labels) {
    this.labels = labels;
  }
}

function componentVariant(kind, value) {
  let variant = Object.create(null);
  variant.kind = componentLabelToJS(kind, true);
  if (value !== undefined) {
    variant.value = value;
  }
  return variant;
}

// Converts the provisional values below, other than a result in return
// position, to the JS values of the JS-API.
function toComponentJSValue(value) {
  if (value instanceof ComponentResult) {
    return componentVariant(value.isOk ? "ok" : "error",
                            toComponentJSValue(value.value));
  }
  if (value instanceof ComponentFlags) {
    return Object.fromEntries(value.labels.map((label) => [componentLabelToJS(label, false), true]));
  }
  if (Array.isArray(value)) {
    return value.map(toComponentJSValue);
  }
  if (typeof value === "object" && value !== null) {
    let result = Object.create(Object.getPrototypeOf(value));
    for (let key of Object.keys(value)) {
      result[key] = toComponentJSValue(value[key]);
    }
    return result;
  }
  return value;
}

function invokeComponent(instance, field, params) {
  let func = instance.exports[componentJSName(field)];
  assertEq(func instanceof Function, true, "expected a function");
  return func(...params.map(toComponentJSValue));
}

// Constructors for component model values. Since the types of values are not
// known, these are an approximation of the JS-API's values: an option is null
// or its payload even when nested, flags are compared by the flags that are
// set, and a list<u8> is compared as a list.
const cv = {
  bool: (x) => x,
  u8: (x) => x,
  s8: (x) => x,
  u16: (x) => x,
  s16: (x) => x,
  u32: (x) => x,
  s32: (x) => x,
  u64: (x) => x,
  s64: (x) => x,
  f32: (x) => Math.fround(x),
  f64: (x) => x,
  char: (x) => x,
  string: (x) => x,
  list: (vals) => vals,
  tuple: (vals) => vals,
  record: (fields) => Object.fromEntries(fields.map(([name, val]) => [componentLabelToJS(name, false), val])),
  variant: (kind, val) => componentVariant(kind, val),
  enum: (label) => label,
  none: () => null,
  some: (val) => val,
  ok: (val) => new ComponentResult(true, val),
  err: (val) => new ComponentResult(false, val),
  flags: (labels) => new ComponentFlags(labels),
};

function componentValuesEqual(actual, expected) {
  if (expected instanceof ComponentFlags) {
    if (typeof actual !== "object" || actual === null) {
      return false;
    }
    let set = Object.keys(actual).filter((key) => actual[key] === true);
    let wanted = expected.labels.map((label) => componentLabelToJS(label, false));
    return set.length === wanted.length && wanted.every((key) => set.includes(key));
  }
  expected = expected instanceof ComponentResult ? toComponentJSValue(expected) : expected;
  if (actual instanceof Uint8Array) {
    actual = Array.from(actual);
  }
  if (typeof actual !== "object" || actual === null || typeof expected !== "object" || expected === null) {
    return Object.is(actual, expected);
  }
  if (Array.isArray(actual) !== Array.isArray(expected)) {
    return false;
  }
  let actualKeys = Object.keys(actual);
  let expectedKeys = Object.keys(expected);
  return actualKeys.length === expectedKeys.length &&
    expectedKeys.every((key) => Object.hasOwn(actual, key) && componentValuesEqual(actual[key], expected[key]));
}

function formatComponentValue(value) {
  return JSON.stringify(value, (_, v) => typeof v === "bigint" ? `${v}n` : v);
}

// Component functions have at most one result. A `result` is unwrapped into
// the return value, or a thrown ComponentError whose data is the error.
function assert_return_component(thunk, expected) {
  assertEq(expected.length <= 1, true, "expected at most one result");
  let wanted = expected.length === 0 ? undefined : expected[0];
  let result;
  if (wanted instanceof ComponentResult && !wanted.isOk) {
    let error = null;
    try {
      thunk();
    } catch (e) {
      error = e;
    }
    assertEq(error instanceof WebAssembly.ComponentError, true, "expected a ComponentError");
    result = error.data;
    wanted = wanted.value;
  } else {
    result = thunk();
    if (wanted instanceof ComponentResult) {
      wanted = wanted.value;
    }
  }
  if (!componentValuesEqual(result, wanted)) {
    assertEq(formatComponentValue(result), formatComponentValue(wanted));
    assertEq(true, false, "component values are not equal");
  }
}
