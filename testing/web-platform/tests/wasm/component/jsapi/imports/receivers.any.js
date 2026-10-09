// META: global=window,dedicatedworker,jsshell
// META: script=/wasm/component/jsapi/harness/component-helpers.js
// META: script=/wasm/component/jsapi/harness/assertions.js

// `[constructor]r`, `[method]r.m` and `[static]r.s` are read from the
// constructor that satisfied the type import `r`, and each is invoked with
// its own receiver rule.

function makeClass(log = []) {
  return class R {
    constructor(x) { log.push(["construct", new.target, x]); this.x = x; }
    m(y) { log.push(["m", this, y]); return this.x + y; }
    static s(z) { log.push(["s", this, z]); return z * 2; }
  };
}

promise_test(async () => {
  const log = [];
  const R = makeClass(log);
  const { make, callM, callS } = await componentExports("imports/receivers", { R });
  const instance = make(3);
  assert_true(instance instanceof R);
  assert_equals(instance.x, 3);
  assert_equals(log.length, 1);
  assert_equals(log[0][0], "construct");
  assert_equals(log[0][1], R, "new.target is the constructor");
  assert_equals(log[0][2], 3);

  assert_equals(callM(instance, 4), 7);
  assert_equals(log[1][0], "m");
  assert_equals(log[1][1], instance, "this is the self argument");
  assert_equals(log[1][2], 4);

  assert_equals(callS(5), 10);
  assert_equals(log[2][0], "s");
  assert_equals(log[2][1], R, "this is the constructor");
  assert_equals(log[2][2], 5);
}, "constructor, method and static receive the right this and new.target");

promise_test(async () => {
  let args;
  class R { m(...rest) { args = rest; return 0; } static s() { return 0; } }
  const { callM } = await componentExports("imports/receivers", { R });
  callM(new R(), 8);
  assert_array_equals(args, [8], "self is not passed as an argument");
}, "a method import receives the arguments after self");

promise_test(async () => {
  const log = [];
  const R = makeClass();
  const imports = loggingProxy({ R, m: () => 0, s: () => 0, make: () => 0,
                                 "[constructor]r": () => 0, "[method]r.m": () => 0, "[static]r.s": () => 0 }, log);
  await componentExports("imports/receivers", imports);
  assert_array_equals(log, ["get R"]);
}, "member imports are not read from the imports object");

promise_test(async () => {
  const log = [];
  const R = makeClass();
  const proto = loggingProxy(R.prototype, log, "proto ");
  // A proxy must report a class's non-writable prototype as is, so proxy a
  // function with the same members instead.
  function Target() {}
  Target.prototype = R.prototype;
  Target.s = R.s;
  const ctor = new Proxy(Target, {
    get(t, key, receiver) {
      log.push(`ctor get ${String(key)}`);
      return key === "prototype" ? proto : Reflect.get(t, key, receiver);
    },
  });
  await componentExports("imports/receivers", { R: ctor });
  assert_array_equals(log, ["ctor get prototype", "proto get Symbol(Symbol.dispose)",
                            "ctor get prototype", "proto get m", "ctor get s"]);
}, "Symbol.dispose is read from R.prototype with the type, then a method from R.prototype and a static from R, in declaration order");

promise_test(async () => {
  class Base { m(y) { return y + 100; } static s(z) { return z + 200; } }
  class R extends Base {}
  const { make, callM, callS } = await componentExports("imports/receivers", { R });
  assert_equals(callM(make(0), 1), 101);
  assert_equals(callS(1), 201);
}, "inherited methods and statics are found on the prototype chain");

test_instantiation_error(WebAssembly.LinkError, "imports/receivers",
                         { R: class { static s() {} } }, undefined,
                         "a missing method on R.prototype is a LinkError");
test_instantiation_error(WebAssembly.LinkError, "imports/receivers",
                         { R: class { m() {} } }, undefined,
                         "a missing static on R is a LinkError");
test_instantiation_error(WebAssembly.LinkError, "imports/receivers",
                         { R: class { m() {} static s = 42; } }, undefined,
                         "a non-callable static is a LinkError");
test_instantiation_error(WebAssembly.LinkError, "imports/receivers",
                         { R: (() => { function F() {} F.prototype = null; F.s = () => 0; return F; })() }, undefined,
                         "R.prototype being null is a LinkError");
test_instantiation_error(WebAssembly.LinkError, "imports/receivers",
                         { R: (() => { function F() {} F.prototype = 1; F.s = () => 0; return F; })() }, undefined,
                         "R.prototype being a primitive is a LinkError");

promise_test(async () => {
  class R {
    constructor() { return {}; }
    m() { return 0; }
    static s() { return 0; }
  }
  const { make } = await componentExports("imports/receivers", { R });
  assert_traps(() => make(1));
}, "a constructor returning an object that fails the brand check traps");

promise_test(async () => {
  const R = makeClass();
  const { make } = await componentExports("imports/receivers", { R });
  const first = make(1);
  const second = make(1);
  assert_not_equals(first, second, "each construction yields a fresh object");
}, "each [constructor] call constructs a new object");

promise_test(async () => {
  const R = makeClass();
  const { callM } = await componentExports("imports/receivers", { R });
  assert_throws_js(TypeError, () => callM({}, 1));
  assert_throws_js(TypeError, () => callM(undefined, 1));
}, "the self argument is brand checked against the constructor before the call");
