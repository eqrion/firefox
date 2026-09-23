// META: global=window,dedicatedworker,jsshell
// META: script=/wasm/component/jsapi/harness/component-helpers.js
// META: script=/wasm/component/jsapi/harness/assertions.js

// `[constructor]r`, `[method]r.add` and `[static]r.create` imports are read
// off the constructor that satisfied the `r` type import, not the imports
// object. The fixture forwards them as construct(x), callAdd(target, n) and
// callCreate(x).

const FIXTURE = "resources/host-members";

function makeClass() {
  const observed = { staticThis: undefined, methodArgs: undefined };
  class C {
    constructor(x) {
      this.x = x;
      this.newTarget = new.target;
      this.ctorArgs = arguments.length;
    }
    add(n) {
      observed.methodArgs = arguments.length;
      this.lastThis = this;
      this.x += n;
      return this.x;
    }
    static create(x) {
      observed.staticThis = this;
      return new C(x + 100);
    }
  }
  return { C, observed };
}

promise_test(async () => {
  const { C } = makeClass();
  const { construct } = await componentExports(FIXTURE, { R: C });
  const c = construct(5);
  assert_equals(Object.getPrototypeOf(c), C.prototype);
  assert_equals(c.x, 5);
  assert_equals(c.newTarget, C, "invoked with Construct");
  assert_equals(c.ctorArgs, 1);
}, "[constructor]: the class is constructed with the converted arguments");

promise_test(async () => {
  const { C } = makeClass();
  const { construct } = await componentExports(FIXTURE, { R: C });
  assert_equals(construct("7").x, 7, "argument converted to u32 before the call");
  assert_throws_js(TypeError, () => construct(1n));
}, "[constructor]: arguments are converted before construction");

promise_test(async () => {
  const { C, observed } = makeClass();
  const { callAdd } = await componentExports(FIXTURE, { R: C });
  const c = new C(1);
  assert_equals(callAdd(c, 3), 4);
  assert_equals(c.x, 4, "mutation through this is visible");
  assert_equals(c.lastThis, c, "this is the borrowed self value");
  assert_equals(observed.methodArgs, 1, "self is not passed as an argument");
}, "[method]: invoked with the self value as this and the remaining params as arguments");

promise_test(async () => {
  const { C, observed } = makeClass();
  const { callCreate } = await componentExports(FIXTURE, { R: C });
  const c = callCreate(1);
  assert_equals(c.x, 101);
  assert_equals(observed.staticThis, C, "this is the constructor");
}, "[static]: invoked with the constructor as this");

promise_test(async () => {
  const { C } = makeClass();
  const log = [];
  const imports = loggingProxy({ R: C }, log);
  await componentExports(FIXTURE, imports);
  assert_array_equals(log, ["get R"]);
}, "member imports are not looked up on the imports object");

promise_test(async () => {
  const { C } = makeClass();
  const { callAdd, callCreate } = await componentExports(FIXTURE, { R: C });
  const originalAdd = C.prototype.add;
  C.prototype.add = function() { return -1; };
  C.create = () => { throw new Error("replaced"); };
  try {
    const c = new C(1);
    assert_equals(callAdd(c, 1), 2, "method captured at link time");
    assert_equals(callCreate(1).x, 101, "static captured at link time");
  } finally {
    C.prototype.add = originalAdd;
  }
}, "member imports are read once at instantiation");

promise_test(async () => {
  const log = [];
  function F() {}
  F.prototype = loggingProxy({ add() {} }, log, "proto ");
  F.create = () => {};
  await componentExports(FIXTURE, { R: F });
  assert_array_equals(log.filter(e => e.startsWith("proto get")),
                      ["proto get Symbol(Symbol.dispose)", "proto get add"]);
}, "[method]: looked up with Get on the prototype");

test_instantiation_error(WebAssembly.LinkError, FIXTURE, { R: class { static create() {} } }, undefined,
                         "[method]: a missing method is a LinkError");

test_instantiation_error(WebAssembly.LinkError, FIXTURE, { R: class { add() {} } }, undefined,
                         "[static]: a missing static is a LinkError");

function withPrototype(prototype) {
  function F() {}
  F.prototype = prototype;
  F.create = () => {};
  return F;
}

test_instantiation_error(WebAssembly.LinkError, FIXTURE, { R: withPrototype({ add: 42 }) }, undefined,
                         "[method]: a non-callable method is a LinkError");

test_instantiation_error(WebAssembly.LinkError, FIXTURE, { R: withPrototype(5) }, undefined,
                         "[method]: a constructor whose prototype is not an object is a LinkError");

test_instantiation_error(WebAssembly.LinkError, FIXTURE, { R: withPrototype(null) }, undefined,
                         "[method]: a constructor whose prototype is null is a LinkError");

test_instantiation_error(WebAssembly.LinkError, FIXTURE,
                         { R: Object.assign(() => {}, { create() {} }) }, undefined,
                         "[constructor]: a callable that is not a constructor is a LinkError");

promise_test(async () => {
  class Base { add() { return 1; } static create(x) { return new Derived(x); } }
  class Derived extends Base {}
  const { callAdd, callCreate } = await componentExports(FIXTURE, { R: Derived });
  assert_equals(callAdd(new Derived(), 0), 1, "inherited method");
  assert_equals(Object.getPrototypeOf(callCreate(1)), Derived.prototype, "inherited static");
}, "members inherited through the prototype chain are found");

promise_test(async () => {
  const { C } = makeClass();
  const { callAdd } = await componentExports(FIXTURE, { R: C });
  assert_throws_js(TypeError, () => callAdd({}, 1));
  assert_throws_js(TypeError, () => callAdd(undefined, 1));
}, "[method]: the self argument is brand checked");
