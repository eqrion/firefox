// META: global=window,dedicatedworker,jsshell
// META: script=/wasm/component/jsapi/harness/component-helpers.js
// META: script=/wasm/component/jsapi/harness/assertions.js

// A resource type exported under several names: one guest class per abstract
// type, shared by every transparent `(eq)` export.

let exports;
promise_setup(async () => {
  exports = await componentExports("resources/guest-transparent");
});

promise_test(async () => {
  assert_exports_object(exports, ["Counter", "Counter2", "iface"]);
  assert_equals(exports.Counter, exports.Counter2);
  assert_exports_object(exports.iface, ["Counter"], "exports.iface");
  assert_equals(exports.iface.Counter, exports.Counter);
}, "transparent type exports resolve to the same class");

promise_test(async () => {
  assert_guest_class(exports.Counter, {
    name: "Counter",
    length: 1,
    methods: [["increment", 0], ["decrement", 0]],
    statics: [["create", 1]],
  });
}, "members declared under either name attach to the one class");

promise_test(async () => {
  const { Counter, Counter2 } = exports;
  const counter = new Counter(5);
  assert_equals(counter.increment(), 6);
  assert_equals(counter.decrement(), 4);
  const created = Counter2.create(3);
  assert_true(created instanceof Counter);
  assert_equals(created.decrement(), 2);
}, "instances work with members from both names");
