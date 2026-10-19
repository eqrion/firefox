// META: global=window,dedicatedworker,jsshell
// META: script=/wasm/component/jsapi/harness/component-helpers.js
// META: script=/wasm/component/jsapi/harness/assertions.js

function makeImports() {
  class R { m() { return 42; } }
  return {
    iface: {
      f: x => x + 1,
      R,
      nested: { g: () => 5 },
    },
    "wasi:clocks/wall-clock@0.2.0": { now: () => 123n },
    outer: { "wasm:js/global": { h: () => 9 } },
  };
}

promise_test(async () => {
  const { callF, makeM, callG, now, callH } = await componentExports("imports/instances", makeImports());
  assert_equals(callF(1), 2);
  assert_equals(makeM(), 42);
  assert_equals(callG(), 5);
  assert_equals(now(), 123n);
  assert_equals(callH(), 9);
}, "instance imports resolve their exports by JS name");

promise_test(async () => {
  const log = [];
  const imports = makeImports();
  const proxied = loggingProxy({
    iface: loggingProxy({ ...imports.iface, nested: loggingProxy(imports.iface.nested, log, "nested ") }, log, "iface "),
    "wasi:clocks/wall-clock@0.2.0": loggingProxy(imports["wasi:clocks/wall-clock@0.2.0"], log, "clock "),
    outer: loggingProxy({ "wasm:js/global": loggingProxy(imports.outer["wasm:js/global"], log, "global ") }, log, "outer "),
  }, log);
  await componentExports("imports/instances", proxied);
  assert_array_equals(log, [
    "get iface",
    "iface get f",
    "iface get R",
    "iface get nested",
    "nested get g",
    "get wasi:clocks/wall-clock@0.2.0",
    "clock get now",
    "get outer",
    "outer get wasm:js/global",
    "global get h",
  ]);
}, "nested lookups happen in declaration order with exact keys");

promise_test(async () => {
  const imports = makeImports();
  imports.outer["wasm:js/global"] = { h: () => 77 };
  const { callH } = await componentExports("imports/instances", imports, { builtins: ["js/global"] });
  assert_equals(callH(), 77);
}, "a builtin-named export inside an instance type is read from the object, not resolved as a builtin");

for (const [value, description] of [
  [undefined, "undefined"], [null, "null"], [1, "number"], ["s", "string"], [Symbol(), "symbol"], [true, "boolean"],
]) {
  test_instantiation_error(WebAssembly.LinkError, "imports/instances", { ...makeImports(), iface: value }, undefined,
                           `a non-object instance import (${description}) is a LinkError`);
}

test_instantiation_error(TypeError, "imports/instances", 1, undefined,
                         "a non-object top-level imports object is a TypeError, unlike an instance import");

promise_test(async () => {
  const imports = makeImports();
  imports.iface = () => {};
  imports.iface.f = x => x;
  imports.iface.R = class { m() { return 1; } };
  imports.iface.nested = { g: () => 1 };
  const { callF } = await componentExports("imports/instances", imports);
  assert_equals(callF(3), 3);
}, "a function is an object and satisfies an instance import");

test_instantiation_error(WebAssembly.LinkError, "imports/instances",
                         (() => { const i = makeImports(); delete i.iface.nested.g; return i; })(), undefined,
                         "a missing export in a nested instance is a LinkError");
test_instantiation_error(WebAssembly.LinkError, "imports/instances",
                         (() => { const i = makeImports(); i.iface.R = {}; return i; })(), undefined,
                         "a non-callable resource type inside an instance is a LinkError");
test_instantiation_error(WebAssembly.LinkError, "imports/instances",
                         (() => { const i = makeImports(); i.iface.R = class {}; return i; })(), undefined,
                         "a resource inside an instance missing its method is a LinkError");
test_instantiation_error(WebAssembly.LinkError, "imports/instances",
                         (() => { const i = makeImports(); delete i["wasi:clocks/wall-clock@0.2.0"]; return i; })(), undefined,
                         "a missing interface-named instance is a LinkError");

promise_test(async () => {
  const imports = makeImports();
  imports["wasi:clocks/wall-clock@0.2.0"] = Object.create({ now: () => 1n });
  const { now } = await componentExports("imports/instances", imports);
  assert_equals(now(), 1n);
}, "instance exports are found on the prototype chain of the instance object");
