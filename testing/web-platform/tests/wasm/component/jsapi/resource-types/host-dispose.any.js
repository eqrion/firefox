// META: global=window,dedicatedworker,jsshell
// META: script=/wasm/component/jsapi/harness/component-helpers.js
// META: script=/wasm/component/jsapi/harness/assertions.js

// Dropping an own handle of a host resource calls the Symbol.dispose of the
// constructor's prototype, which is read when the instance is created.

const FIXTURE = "resources/host-dispose";

function makeClass() {
  const disposed = [];
  class C {
    [Symbol.dispose]() { disposed.push(this); }
  }
  return { C, disposed };
}

promise_test(async () => {
  const { C, disposed } = makeClass();
  const { dropOwn } = await componentExports(FIXTURE, { R: C });
  const c = new C();
  dropOwn(c);
  assert_equals(disposed.length, 1, "called once");
  assert_equals(disposed[0], c, "called with the value as this");
}, "dropping an own handle calls Symbol.dispose with the value as this");

promise_test(async () => {
  const { C, disposed } = makeClass();
  const { keep, dropKept } = await componentExports(FIXTURE, { R: C });
  const c = new C();
  keep(c);
  assert_equals(disposed.length, 0, "not called while the guest holds the handle");
  dropKept();
  assert_array_equals(disposed, [c]);
}, "Symbol.dispose is called when the guest drops the handle, not when it is passed");

promise_test(async () => {
  const { C, disposed } = makeClass();
  const { dropBorrow, giveBack } = await componentExports(FIXTURE, { R: C });
  const c = new C();
  dropBorrow(c);
  assert_equals(giveBack(c), c);
  assert_equals(disposed.length, 0);
}, "dropping a borrow, or passing an own back to JS, does not call Symbol.dispose");

promise_test(async () => {
  const { C, disposed } = makeClass();
  const original = C.prototype[Symbol.dispose];
  const log = [];
  const { dropOwn } = await componentExports(FIXTURE, { R: C });
  C.prototype[Symbol.dispose] = function () { log.push("replaced"); };
  try {
    dropOwn(new C());
  } finally {
    C.prototype[Symbol.dispose] = original;
  }
  assert_equals(disposed.length, 1, "the original method is called");
  assert_array_equals(log, [], "the replacement is not called");
}, "Symbol.dispose is read from the prototype at instantiation");

promise_test(async () => {
  const { C } = makeClass();
  let called = 0;
  const c = new C();
  c[Symbol.dispose] = () => { called++; };
  const { dropOwn } = await componentExports(FIXTURE, { R: C });
  dropOwn(c);
  assert_equals(called, 0, "the value's own property is not consulted");
}, "Symbol.dispose is not read from the value");

for (const [make, description] of [
  [() => { class C {} return C; }, "no Symbol.dispose"],
  [() => { class C {} C.prototype[Symbol.dispose] = 1; return C; }, "a non-callable Symbol.dispose"],
  [() => { const f = () => {}; return f; }, "a constructor without a prototype"],
]) {
  promise_test(async () => {
    const C = make();
    const { dropOwn, ok } = await componentExports(FIXTURE, { R: C });
    const value = C.prototype ? new C() : Object.create(Object.prototype);
    if (!C.prototype) {
      Object.defineProperty(C, Symbol.hasInstance, { value: () => true });
    }
    dropOwn(value);
    assert_equals(ok(), 1);
  }, `dropping an own handle with ${description} does nothing`);
}

promise_test(async () => {
  class C {
    [Symbol.dispose]() { throw new Error("from dispose"); }
  }
  const { dropOwn, ok } = await componentExports(FIXTURE, { R: C });
  assert_traps(() => dropOwn(new C()));
  assert_traps(() => ok(), "the instance is locked down");
}, "a throwing Symbol.dispose traps and locks the instance down");

promise_test(async () => {
  const log = [];
  function F() {}
  F.prototype = loggingProxy({ [Symbol.dispose]() {} }, log, "proto ");
  await componentExports(FIXTURE, { R: F });
  assert_array_equals(log, ["proto get Symbol(Symbol.dispose)"]);
}, "Symbol.dispose is read once with Get");
