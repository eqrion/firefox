// META: global=window,dedicatedworker,jsshell
// META: script=/wasm/component/jsapi/harness/component-helpers.js
// META: script=/wasm/component/jsapi/harness/assertions.js

// `[get]`/`[set]` imports resolve with `find an accessor`: the property must
// be an accessor, found on the target or its prototype chain, and the getter
// or setter is called with the target as receiver.

function makeImports(log = []) {
  class R {
    get p() { log.push(["get p", this]); return this._p ?? 1; }
    set p(v) { log.push(["set p", this, v]); this._p = v; }
    static get q() { log.push(["get q", this]); return R._q ?? 2; }
    static set q(v) { log.push(["set q", this, v]); R._q = v; }
  }
  const imports = {
    get x() { log.push(["get x", this]); return imports._x ?? 3; },
    set x(v) { log.push(["set x", this, v]); imports._x = v; },
    R,
  };
  return { imports, R, log };
}

// An imports object with a plain `x` accessor pair plus `extra`.
function withX(extra) {
  const imports = {
    get x() { return imports._x ?? 3; },
    set x(v) { imports._x = v; },
    ...extra,
  };
  return imports;
}

promise_test(async () => {
  const { imports, R, log } = makeImports();
  const exports = await componentExports("imports/accessors", imports);
  assert_equals(exports.getX(), 3);
  assert_equals(log.at(-1)[0], "get x");
  assert_equals(log.at(-1)[1], imports, "receiver is the imports object");
  exports.setX(30);
  assert_equals(log.at(-1)[0], "set x");
  assert_equals(log.at(-1)[1], imports);
  assert_equals(log.at(-1)[2], 30);
  assert_equals(exports.getX(), 30);

  const instance = new R();
  assert_equals(exports.getP(instance), 1);
  assert_equals(log.at(-1)[1], instance, "receiver is self");
  exports.setP(instance, 10);
  assert_equals(instance._p, 10);
  assert_equals(exports.getP(instance), 10);

  assert_equals(exports.getQ(), 2);
  assert_equals(log.at(-1)[1], R, "receiver is the constructor");
  exports.setQ(20);
  assert_equals(R._q, 20);
  assert_equals(exports.getQ(), 20);
}, "getters and setters are invoked with the right receiver");

promise_test(async () => {
  const { imports, log } = makeImports();
  await componentExports("imports/accessors", imports);
  assert_array_equals(log, [], "accessors are looked up, not invoked, at instantiation");
}, "reading an accessor import does not invoke it");

promise_test(async () => {
  const { imports } = makeImports();
  const log = [];
  await componentExports("imports/accessors", loggingProxy(imports, log));
  assert_array_equals(log.filter(e => !e.startsWith("getPrototypeOf")), [
    "getOwnPropertyDescriptor x",
    "getOwnPropertyDescriptor x",
    "get R",
  ]);
}, "top-level accessor imports are found with [[GetOwnProperty]], not [[Get]]");

promise_test(async () => {
  const { imports, R } = makeImports();
  const base = { get x() { return 7; }, set x(v) { base._x = v; } };
  const derived = Object.create(base);
  derived.R = R;
  const exports = await componentExports("imports/accessors", derived);
  assert_equals(exports.getX(), 7);
  exports.setX(8);
  assert_equals(base._x, 8);
}, "an accessor on the prototype chain is found");

promise_test(async () => {
  class Base { get p() { return 11; } set p(v) { this._p = v; } static get q() { return 12; } static set q(v) {} }
  class R extends Base {}
  const exports = await componentExports("imports/accessors", withX({ R }));
  assert_equals(exports.getP(new R()), 11);
  assert_equals(exports.getQ(), 12);
}, "inherited class accessors are found");

test_instantiation_error(WebAssembly.LinkError, "imports/accessors",
                         (() => { const { R } = makeImports(); return { x: 5, R }; })(), undefined,
                         "a data property for [get]x is a LinkError");

test_instantiation_error(WebAssembly.LinkError, "imports/accessors",
                         (() => {
                           const { imports, R } = makeImports();
                           const shadowed = Object.create(imports);
                           Object.defineProperty(shadowed, "x", { value: 1 });
                           shadowed.R = R;
                           return shadowed;
                         })(), undefined,
                         "a data property shadowing an accessor further up the chain is a LinkError");

test_instantiation_error(WebAssembly.LinkError, "imports/accessors",
                         (() => { const { R } = makeImports(); return { get x() { return 1; }, R }; })(), undefined,
                         "a getter-only accessor for a [set] import is a LinkError");

test_instantiation_error(WebAssembly.LinkError, "imports/accessors",
                         (() => { const { R } = makeImports(); return { set x(v) {}, R }; })(), undefined,
                         "a setter-only accessor for a [get] import is a LinkError");

test_instantiation_error(WebAssembly.LinkError, "imports/accessors",
                         withX({ R: class { p = 1; static q = 2; } }), undefined,
                         "data properties for resource accessors are a LinkError");

test_instantiation_error(WebAssembly.LinkError, "imports/accessors",
                         withX({ R: class { get p() { return 1; } set p(v) {} static get q() { return 2; } } }),
                         undefined,
                         "a missing static setter is a LinkError");

promise_test(async () => {
  const { R } = makeImports();
  const log = [];
  const proto = loggingProxy(R.prototype, log, "proto ");
  // A proxy must report a class's non-writable prototype as is, so proxy a
  // function with the same members instead.
  function Target() {}
  Target.prototype = R.prototype;
  Object.defineProperty(Target, "q", Object.getOwnPropertyDescriptor(R, "q"));
  const ctor = new Proxy(Target, {
    get(t, key, receiver) {
      log.push(`ctor get ${String(key)}`);
      return key === "prototype" ? proto : Reflect.get(t, key, receiver);
    },
    getOwnPropertyDescriptor(t, key) {
      log.push(`ctor getOwnPropertyDescriptor ${String(key)}`);
      return Reflect.getOwnPropertyDescriptor(t, key);
    },
  });
  await componentExports("imports/accessors", withX({ R: ctor }));
  assert_array_equals(log.filter(e => !e.includes("getPrototypeOf")), [
    "ctor get prototype", "proto get Symbol(Symbol.dispose)",
    "ctor get prototype", "proto getOwnPropertyDescriptor p",
    "ctor get prototype", "proto getOwnPropertyDescriptor p",
    "ctor getOwnPropertyDescriptor q",
    "ctor getOwnPropertyDescriptor q",
  ]);
}, "resource accessors are found with [[GetOwnProperty]] on R.prototype and R");

promise_test(async () => {
  const { imports } = makeImports();
  const exports = await componentExports("imports/accessors", imports);
  assert_throws_js(TypeError, () => exports.getP({}));
  assert_throws_js(TypeError, () => exports.setP(null, 1));
}, "self is brand checked for method accessors");

promise_test(async () => {
  const { R } = makeImports();
  let boom = false;
  const imports = {
    get x() { if (boom) throw new Error("boom"); return 1; },
    set x(v) {},
    R,
  };
  const exports = await componentExports("imports/accessors", imports);
  assert_equals(exports.getX(), 1);
  boom = true;
  assert_traps(() => exports.getX());
}, "a getter throwing where the type is not a result traps");
