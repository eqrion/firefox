// META: global=window,dedicatedworker,jsshell
// META: script=/wasm/component/jsapi/harness/component-helpers.js
// META: script=/wasm/component/jsapi/harness/assertions.js

// Which error class each kind of bad import produces, checked through every
// instantiation entry point. TypeError comes from the imports object itself or
// a value conversion, LinkError from an import value of the wrong shape, and
// RuntimeError from a trap during instantiation.

class Counter {}

const cases = [
  // [description, expected error, fixture, imports, options]
  ["imports object is undefined", TypeError, "instance/echo", undefined],
  ["imports object is null", TypeError, "instance/echo", null],
  ["imports object is a number", TypeError, "instance/echo", 1],
  ["imports object is a string", TypeError, "instance/echo", "echo"],
  ["value import is undefined", TypeError, "instance/value-import", { v: undefined }],
  ["value import is missing", TypeError, "instance/value-import", {}],
  ["value import is out of range", TypeError, "instance/value-import", { v: -1 }],
  ["value import is NaN", TypeError, "instance/value-import", { v: NaN }],
  ["value import is a Symbol", TypeError, "instance/value-import", { v: Symbol() }],
  ["value import is a BigInt", TypeError, "instance/value-import", { v: 1n }],
  ["method export named constructor", TypeError, "instance/reserved-method-constructor", {}],
  ["static export named prototype", TypeError, "instance/reserved-static-prototype", {}],
  ["public re-export of an imported resource type", TypeError, "instance/reexport-host-type",
   { Counter: Counter }],

  ["function import is missing", WebAssembly.LinkError, "instance/echo", {}],
  ["function import is undefined", WebAssembly.LinkError, "instance/echo", { echo: undefined }],
  ["function import is null", WebAssembly.LinkError, "instance/echo", { echo: null }],
  ["function import is a number", WebAssembly.LinkError, "instance/echo", { echo: 1 }],
  ["function import is a string", WebAssembly.LinkError, "instance/echo", { echo: "x => x" }],
  ["function import is a plain object", WebAssembly.LinkError, "instance/echo", { echo: {} }],
  ["function import is a class instance", WebAssembly.LinkError, "instance/echo", { echo: new Counter() }],
  ["function import is a Module", WebAssembly.LinkError, "instance/echo",
   { echo: new WebAssembly.Module(emptyModuleBytes) }],
  ["type import is missing", WebAssembly.LinkError, "instance/type-import", {}],
  ["type import is undefined", WebAssembly.LinkError, "instance/type-import", { Counter: undefined }],
  ["type import is a plain object", WebAssembly.LinkError, "instance/type-import", { Counter: {} }],
  ["type import is a prototype object", WebAssembly.LinkError, "instance/type-import",
   { Counter: Counter.prototype }],
  ["type import is a string", WebAssembly.LinkError, "instance/type-import", { Counter: "Counter" }],
  ["constructor import is not a constructor", WebAssembly.LinkError, "instance/ctor-import",
   { Counter: () => {} }],
  ["constructor import is a bound arrow function", WebAssembly.LinkError, "instance/ctor-import",
   { Counter: (() => {}).bind(null) }],
  ["constructor import is a method", WebAssembly.LinkError, "instance/ctor-import",
   { Counter: ({ m() {} }).m }],
  ["instance import is missing", WebAssembly.LinkError, "instance/instance-import", {}],
  ["instance import is undefined", WebAssembly.LinkError, "instance/instance-import", { iface: undefined }],
  ["instance import is null", WebAssembly.LinkError, "instance/instance-import", { iface: null }],
  ["instance import is a number", WebAssembly.LinkError, "instance/instance-import", { iface: 1 }],
  ["instance import is a string", WebAssembly.LinkError, "instance/instance-import", { iface: "echo" }],
  ["instance import is missing a function", WebAssembly.LinkError, "instance/instance-import", { iface: {} }],
  ["instance import has a non-callable function", WebAssembly.LinkError, "instance/instance-import",
   { iface: { echo: 1 } }],
  ["core module import is missing", WebAssembly.LinkError, "instance/module-import", {}],
  ["core module import is undefined", WebAssembly.LinkError, "instance/module-import", { m: undefined }],
  ["core module import is a plain object", WebAssembly.LinkError, "instance/module-import", { m: {} }],
  ["core module import is a Module.prototype", WebAssembly.LinkError, "instance/module-import",
   { m: WebAssembly.Module.prototype }],
  ["core module import is an Instance", WebAssembly.LinkError, "instance/module-import",
   { m: new WebAssembly.Instance(new WebAssembly.Module(emptyModuleBytes)) }],
  ["core module import is a Component", WebAssembly.LinkError, "instance/module-import",
   { m: new WebAssembly.Component(emptyComponentBytes) }],
  ["core module import lacks the expected export", WebAssembly.LinkError, "instance/module-import",
   { m: new WebAssembly.Module(emptyModuleBytes) }],

  ["start function traps", WebAssembly.RuntimeError, "instance/start-trap", { recv() {} }],
];

for (const [description, error, fixture, imports, options] of cases) {
  test_instantiation_error(error, fixture, imports, options, description);
}

// The fixtures above do instantiate with well-formed imports, so the errors
// are caused by the imports and not the fixtures.
const good = [
  ["instance/echo", { echo: x => x }, "a function"],
  ["instance/value-import", { v: 1 }, "a number"],
  ["instance/type-import", { Counter: Counter }, "a class"],
  ["instance/type-import", { Counter: () => {} }, "an arrow function"],
  ["instance/ctor-import", { Counter: Counter }, "a class"],
  ["instance/ctor-import", { Counter: function () {} }, "a function expression"],
  ["instance/instance-import", { iface: { echo: x => x } }, "an object"],
  ["instance/instance-import", { iface: Object.assign(() => {}, { echo: x => x }) }, "a function with properties"],
  ["instance/start", { recv() {} }, "a function"],
];

for (const [fixture, imports, description] of good) {
  for_each_entry_point(async (t, instantiate) => {
    const instance = await instantiate(fixture, imports);
    assert_equals(Object.getPrototypeOf(instance), WebAssembly.ComponentInstance.prototype);
  }, `${fixture} instantiates with ${description}`);
}

promise_test(async () => {
  const bytes = await componentBytes("instance/echo");
  const component = new WebAssembly.Component(bytes);
  for (const [imports, error] of [[undefined, TypeError], [{}, WebAssembly.LinkError]]) {
    const fromBytes = WebAssembly.instantiate(bytes, imports);
    assert_true(fromBytes instanceof Promise, "instantiate(bytes) returns a promise");
    const fromComponent = WebAssembly.instantiate(component, imports);
    assert_true(fromComponent instanceof Promise, "instantiate(Component) returns a promise");
    for (const promise of [fromBytes, fromComponent]) {
      let thrown;
      try {
        await promise;
      } catch (e) {
        thrown = e;
      }
      assert_equals(Object.getPrototypeOf(thrown), error.prototype);
    }
  }
}, "bad imports reject the promise instead of throwing synchronously");
