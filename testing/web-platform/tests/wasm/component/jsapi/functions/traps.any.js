// META: global=window,dedicatedworker,jsshell
// META: script=/wasm/component/jsapi/harness/component-helpers.js
// META: script=/wasm/component/jsapi/harness/assertions.js

// A trap locks the instance down: every later call into any export throws
// WebAssembly.RuntimeError.

promise_test(async () => {
  const { trap, ok } = await componentExports("functions/traps", { plain: () => 1 });
  assert_equals(ok(), 1);
  assert_traps(() => trap());
}, "a guest unreachable throws RuntimeError");

promise_test(async () => {
  const { trap, ok, callPlain } = await componentExports("functions/traps", { plain: () => 1 });
  assert_traps(() => trap());
  assert_traps(() => ok(), "ok after trap");
  assert_traps(() => callPlain(), "callPlain after trap");
  assert_traps(() => trap(), "trap after trap");
}, "after a trap every export throws RuntimeError");

promise_test(async () => {
  let calls = 0;
  const { trap, callPlain } = await componentExports("functions/traps", { plain: () => { calls++; return 1; } });
  assert_traps(() => trap());
  assert_traps(() => callPlain());
  assert_equals(calls, 0, "imports are not reached from a locked-down instance");
}, "a locked-down instance does not run guest code or imports");

promise_test(async () => {
  const { callPlain, ok } = await componentExports("functions/traps", { plain: () => 1n });
  assert_traps(() => callPlain());
  assert_traps(() => ok(), "ok after conversion trap");
}, "a failed import result conversion is a trap and locks the instance down");

promise_test(async () => {
  const { callPlain, ok } = await componentExports("functions/traps", { plain: () => { throw new Error("x"); } });
  assert_traps(() => callPlain());
  assert_traps(() => ok(), "ok after import throw");
}, "an import throwing through a non-result type locks the instance down");

promise_test(async () => {
  const component = await compileComponent("functions/traps");
  const a = new WebAssembly.ComponentInstance(component, { plain: () => 1 });
  const b = new WebAssembly.ComponentInstance(component, { plain: () => 1 });
  assert_traps(() => a.exports.trap());
  assert_equals(b.exports.ok(), 1, "other instances are unaffected");
  assert_equals(b.exports.ok(), 2);
}, "a trap only affects the trapping instance");

promise_test(async () => {
  const { trap } = await componentExports("functions/traps", { plain: () => 1 });
  let error;
  try {
    trap();
  } catch (e) {
    error = e;
  }
  assert_equals(Object.getPrototypeOf(error), WebAssembly.RuntimeError.prototype);
  assert_equals(typeof error.message, "string");
}, "the trap error is a WebAssembly.RuntimeError with a message");

// A conversion failure locks the instance down only once the call has entered
// the guest, by running realloc or touching a handle table.

promise_test(async () => {
  const { flat, reallocs } = await componentExports("functions/lockdown-conversion");
  assert_throws_js(TypeError, () => flat(1n));
  assert_throws_js(TypeError, () => flat(Symbol()));
  assert_equals(flat(3), 3, "the instance is still usable");
  assert_equals(reallocs(), 0, "no guest code ran");
}, "a bad argument to a flat-only function does not lock the instance down");

promise_test(async () => {
  const { takePoint, flat, reallocs } = await componentExports("functions/lockdown-conversion");
  assert_throws_js(TypeError, () => takePoint({ s: Symbol(), n: 1 }));
  assert_equals(reallocs(), 0, "ToString threw before realloc ran");
  assert_equals(takePoint({ s: "ok", n: 4 }), 4, "the instance is still usable");
  assert_equals(flat(1), 1);
}, "a conversion failure before the guest is entered does not lock the instance down");

promise_test(async () => {
  const { takePoint, flat, reallocs } = await componentExports("functions/lockdown-conversion");
  assert_equals(reallocs(), 0);
  assert_throws_js(TypeError, () => takePoint({ s: "ok", n: 1n }), "the failure is still a TypeError");
  assert_traps(() => flat(1), "flat after a failed conversion");
  assert_traps(() => reallocs(), "reallocs after a failed conversion");
  assert_traps(() => takePoint({ s: "ok", n: 1 }), "takePoint after a failed conversion");
}, "a conversion failure after realloc ran locks the instance down");

promise_test(async () => {
  const { takePoint, flat } = await componentExports("functions/lockdown-conversion");
  const log = [];
  const point = { get s() { log.push("s"); return "ok"; }, get n() { log.push("n"); throw new Error("from n"); } };
  assert_throws_js(Error, () => takePoint(point));
  assert_array_equals(log, ["s", "n"], "fields are read in declaration order");
  assert_traps(() => flat(1));
}, "a getter throwing after the guest is entered locks the instance down and propagates as-is");
