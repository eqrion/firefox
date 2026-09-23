// Helpers for loading the component fixtures under components/ and
// instantiating them through every JS-API entry point.

const COMPONENT_ROOT = "/wasm/component/jsapi/components/";

// \0asm, version 13, layer 1.
const COMPONENT_HEADER = [0x00, 0x61, 0x73, 0x6d, 0x0d, 0x00, 0x01, 0x00];
// \0asm, version 1, layer 0.
const MODULE_HEADER = [0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00];

const emptyModuleBytes = new Uint8Array(MODULE_HEADER);
const emptyComponentBytes = new Uint8Array(COMPONENT_HEADER);

function componentURL(name) {
  return `${COMPONENT_ROOT}${name}.wasm`;
}

const bytesCache = new Map();

// Fetches components/<name>.wasm once and caches the bytes. Callers must not
// mutate the returned array; use componentBytesCopy for that.
function componentBytes(name) {
  if (!bytesCache.has(name)) {
    bytesCache.set(name, (async () => {
      const response = await fetch(componentURL(name));
      if (!response.ok) {
        throw new Error(`failed to fetch ${name}: ${response.status}. Did you run build.py?`);
      }
      return new Uint8Array(await response.arrayBuffer());
    })());
  }
  return bytesCache.get(name);
}

async function componentBytesCopy(name) {
  return new Uint8Array(await componentBytes(name));
}

async function compileComponent(name, options) {
  return new WebAssembly.Component(await componentBytes(name), options);
}

async function instantiateComponent(name, imports, options) {
  return new WebAssembly.ComponentInstance(await compileComponent(name, options), imports);
}

async function componentExports(name, imports, options) {
  return (await instantiateComponent(name, imports, options)).exports;
}

// Every way of turning bytes into a ComponentInstance. Each entry is
// [description, async (bytes, imports, options) => ComponentInstance].
const instantiationEntryPoints = [
  [
    "new WebAssembly.ComponentInstance",
    async (bytes, imports, options) =>
      new WebAssembly.ComponentInstance(new WebAssembly.Component(bytes, options), imports),
  ],
  [
    "WebAssembly.instantiate(bytes)",
    async (bytes, imports, options) =>
      (await WebAssembly.instantiate(bytes, imports, options)).instance,
  ],
  [
    "WebAssembly.instantiate(Component)",
    async (bytes, imports, options) =>
      WebAssembly.instantiate(new WebAssembly.Component(bytes, options), imports),
  ],
];

// Runs `body(instantiate)` once per entry point as separate promise_tests.
// `instantiate(name, imports, options)` resolves to a ComponentInstance.
function for_each_entry_point(body, description) {
  for (const [entryName, entry] of instantiationEntryPoints) {
    promise_test(async t => {
      const instantiate = async (name, imports, options) =>
        entry(await componentBytes(name), imports, options);
      await body(t, instantiate);
    }, `${description} [${entryName}]`);
  }
}

// Asserts that instantiating `name` fails with `errorCtor` through every entry
// point. Synchronous throws and rejections are both accepted, since the
// constructor path throws and the promise paths reject.
function test_instantiation_error(errorCtor, name, imports, options, description) {
  for_each_entry_point(async (t, instantiate) => {
    await promise_rejects_js(t, errorCtor, (async () => instantiate(name, imports, options))());
  }, description);
}

// A trap surfaces as WebAssembly.RuntimeError.
function assert_traps(fn, description) {
  assert_throws_js(WebAssembly.RuntimeError, fn, description);
}

// Records every property access on an imports object so tests can assert the
// exact keys and order the JS-API reads.
function loggingProxy(target, log, label = "") {
  return new Proxy(target, {
    get(t, key, receiver) {
      log.push(`${label}get ${String(key)}`);
      return Reflect.get(t, key, receiver);
    },
    getOwnPropertyDescriptor(t, key) {
      log.push(`${label}getOwnPropertyDescriptor ${String(key)}`);
      return Reflect.getOwnPropertyDescriptor(t, key);
    },
    getPrototypeOf(t) {
      log.push(`${label}getPrototypeOf`);
      return Reflect.getPrototypeOf(t);
    },
    has(t, key) {
      log.push(`${label}has ${String(key)}`);
      return Reflect.has(t, key);
    },
  });
}
