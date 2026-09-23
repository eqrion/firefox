// META: global=window,dedicatedworker,jsshell
// META: script=/wasm/component/jsapi/harness/component-helpers.js
// META: script=/wasm/component/jsapi/harness/assertions.js

// Exports of one instance passed as imports to another are plain JS values.
// There is no direct linking: every value crosses through the JS conversions
// of both components.

async function link(overrides = {}) {
  const a = await componentExports("imports/through-js-a");
  const imports = { add: a.add, Counter: a.Counter, iface: a.iface, ...overrides };
  const b = await componentExports("imports/through-js-b", imports);
  return { a, b };
}

promise_test(async () => {
  const { b } = await link();
  assert_equals(b.run(3), 3 + 1 + 3 * 2 + 3);
}, "a function, resource class and instance export link through JS");

promise_test(async () => {
  const seen = [];
  const a = await componentExports("imports/through-js-a");
  const b = await componentExports("imports/through-js-b", {
    add: (x, y) => { seen.push([x, y]); return 0; },
    Counter: a.Counter,
    iface: a.iface,
  });
  b.run(2);
  assert_value_equals(seen.at(-1), [2, 1], "the JS wrapper observes converted JS values");
}, "a JS wrapper around an exported function sees ordinary JS arguments");

promise_test(async () => {
  const { a } = await link();
  let constructed;
  class Spy extends a.Counter {
    constructor(start) { super(start); constructed = this; }
  }
  const b = await componentExports("imports/through-js-b", { add: a.add, Counter: Spy, iface: a.iface });
  assert_equals(b.run(4), 4 + 1 + 8 + 4);
  assert_true(constructed instanceof Spy, "B constructed the JS class via Construct");
}, "a subclass of an exported resource class satisfies the type import");

promise_test(async () => {
  const { a } = await link();
  const b = await componentExports("imports/through-js-b", {
    add: a.add, Counter: a.Counter,
    iface: { mul: (x, y) => a.iface.mul(x, y) * 10 },
  });
  assert_equals(b.run(1), 1 + 1 + 20 + 1);
}, "an instance import may be an ordinary object wrapping another instance's exports");

promise_test(async () => {
  const { a } = await link();
  const b = await componentExports("imports/through-js-b", { add: a.add, Counter: a.Counter, iface: a.iface });
  const other = await componentExports("imports/through-js-a");
  const c = await componentExports("imports/through-js-b", { add: other.add, Counter: other.Counter, iface: other.iface });
  assert_equals(b.run(1), c.run(1));
}, "two instances of the producer link independently");

promise_test(async t => {
  const a = await componentExports("imports/through-js-a");
  await promise_rejects_js(t, WebAssembly.LinkError, (async () =>
    new WebAssembly.ComponentInstance(await compileComponent("imports/through-js-b"),
                                      { add: a.add, Counter: a.Counter, iface: a.add }))());
}, "an exported function used as an instance import lacks the instance's exports and is a LinkError");

promise_test(async t => {
  const a = await componentExports("imports/through-js-a");
  const bytes = await componentBytes("imports/through-js-b");
  await promise_rejects_js(t, WebAssembly.LinkError,
                           WebAssembly.instantiate(bytes, { add: a.add, Counter: a.iface, iface: a.iface }));
}, "an exports object does not satisfy a resource type import");
