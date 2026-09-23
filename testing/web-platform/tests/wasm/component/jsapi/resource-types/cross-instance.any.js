// META: global=window,dedicatedworker,jsshell
// META: script=/wasm/component/jsapi/harness/component-helpers.js
// META: script=/wasm/component/jsapi/harness/assertions.js

// Guest resource classes belong to one component instance. Two instantiations
// of the same Component have distinct classes whose instances do not mix,
// whereas host resource values are plain JS objects usable anywhere.

const GUEST = "resources/host-guest-source";
const HOST = "resources/host-brand-check";

promise_test(async () => {
  const a = await componentExports(GUEST);
  const b = await componentExports(GUEST);
  assert_guest_class(a.Counter, { name: "Counter", length: 1, methods: [["increment", 0]] });
  assert_not_equals(a.Counter, b.Counter, "classes");
  assert_not_equals(a.Counter.prototype, b.Counter.prototype, "prototypes");
  assert_not_equals(a.take, b.take, "functions");
  assert_not_equals(a, b, "exports objects");
}, "two instantiations produce distinct classes, prototypes, functions and exports objects");

promise_test(async () => {
  const a = await componentExports(GUEST);
  const b = await componentExports(GUEST);
  const counter = new a.Counter(1);
  assert_false(counter instanceof b.Counter);
  assert_equals(a.Counter.prototype.increment.call(counter), 2, "own class method");
  assert_throws_js(TypeError, () => b.Counter.prototype.increment.call(counter), "other class method");
  assert_throws_js(TypeError, () => b.take(counter), "other instance's export");
  assert_equals(a.take(counter), 1, "own instance's export");
}, "an instance of one instantiation's class is rejected by the other instantiation");

promise_test(async () => {
  const a = await componentExports(GUEST);
  const b = await componentExports(GUEST);
  const host = await componentExports(HOST, { R: a.Counter, make: () => new a.Counter(0) });
  const fromA = new a.Counter(1);
  const fromB = new b.Counter(1);
  assert_equals(host.identity(fromA), fromA);
  assert_throws_js(TypeError, () => host.identity(fromB));
}, "a guest class used as a host type import only accepts that instantiation's instances");

promise_test(async () => {
  class C {}
  const x = await componentExports(HOST, { R: C, make: () => new C() });
  const y = await componentExports(HOST, { R: C, make: () => new C() });
  const c = new C();
  assert_equals(x.identity(c), c);
  assert_equals(y.identity(c), c);
  assert_equals(y.identity(x.identity(c)), c);
}, "host resource values are not bound to an instance");

promise_test(async () => {
  const a = await componentExports(GUEST);
  const first = new a.Counter(1);
  const second = new a.Counter(1);
  assert_not_equals(first, second);
  assert_equals(Object.getPrototypeOf(first), Object.getPrototypeOf(second));
}, "instances of one class share a prototype but are distinct objects");
