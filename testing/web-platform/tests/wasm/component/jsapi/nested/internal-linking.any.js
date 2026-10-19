// META: global=window,dedicatedworker,jsshell
// META: script=/wasm/component/jsapi/harness/component-helpers.js
// META: script=/wasm/component/jsapi/harness/assertions.js

// Two inner components linked directly: $A defines a resource, $B consumes
// it and a host resource threaded through the outer component's imports.

class Host {
  constructor() { this.pings = 0; }
  ping() { return ++this.pings; }
}

async function load() {
  return componentExports("nested/internal-linking", { Host });
}

promise_test(async () => {
  const instance = await instantiateComponent("nested/internal-linking", { Host });
  assert_ComponentInstance(instance, ["Counter", "dropCount", "makeAndIncrement", "pingTwice"]);
}, "the outer exports object exposes definitions from both inner instances");

promise_test(async () => {
  const { Counter } = await load();
  assert_guest_class(Counter, { name: "Counter", length: 1, methods: [["increment", 0]] });
}, "a resource defined in an inner component is a guest resource class");

promise_test(async () => {
  const { Counter, dropCount } = await load();
  const counter = new Counter(10);
  assert_equals(counter.increment(), 11);
  assert_equals(counter.increment(), 12);
  assert_equals(dropCount(), 0);
  counter[Symbol.dispose]();
  assert_equals(dropCount(), 1);
}, "the inner resource's constructor, method and destructor work through the outer");

promise_test(async () => {
  const { makeAndIncrement, dropCount } = await load();
  assert_equals(makeAndIncrement(5), 7);
  assert_equals(dropCount(), 1, "the inner component dropped its own handle");
}, "an inner component calls another inner component's exports directly");

promise_test(async () => {
  const { pingTwice } = await load();
  const host = new Host();
  assert_equals(pingTwice(host), 3);
  assert_equals(host.pings, 2);
}, "a host resource threads through an inner component's import");

promise_test(async () => {
  const { pingTwice } = await load();
  assert_throws_js(TypeError, () => pingTwice({}));
  assert_throws_js(TypeError, () => pingTwice(Object.create(null)));
}, "a host resource imported by an inner component is brand checked at the outer boundary");

promise_test(async () => {
  const a = await load();
  const b = await load();
  const counter = new a.Counter(0);
  assert_throws_js(TypeError, () => b.Counter.prototype.increment.call(counter));
}, "inner resource classes are per outer instance");
