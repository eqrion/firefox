// META: global=window,dedicatedworker,jsshell
// META: script=/wasm/component/jsapi/harness/component-helpers.js
// META: script=/wasm/component/jsapi/harness/assertions.js

// Synchronous re-entrance: an import may call back into the instance that is
// calling it.

promise_test(async () => {
  let exports;
  exports = await componentExports("functions/reentrance", {
    cb: n => exports.inner() + n,
  });
  assert_equals(exports.outer(10), 12, "inner returned 1, plus n, plus 1 from outer");
  assert_equals(exports.inner(), 2);
}, "an import may call a different export of the same instance");

promise_test(async () => {
  let exports;
  exports = await componentExports("functions/reentrance", {
    cb: n => (n === 0 ? 0 : exports.outer(n - 1)),
  });
  assert_equals(exports.outer(5), 6);
  assert_equals(exports.maxDepth(), 6);
}, "an import may re-enter the export that is calling it");

promise_test(async () => {
  const other = await componentExports("functions/reentrance", { cb: n => n });
  const exports = await componentExports("functions/reentrance", {
    cb: n => other.outer(n),
  });
  assert_equals(exports.outer(1), 3);
  assert_equals(other.maxDepth(), 1);
  assert_equals(exports.maxDepth(), 1);
}, "an import may call into a different instance");

promise_test(async () => {
  let exports;
  const seen = [];
  exports = await componentExports("functions/reentrance", {
    cb(n) {
      seen.push(n);
      if (n > 0) exports.outer(n - 1);
      return 0;
    },
  });
  exports.outer(3);
  assert_array_equals(seen, [3, 2, 1, 0]);
}, "nested re-entrant calls run in order");

promise_test(async () => {
  let exports;
  exports = await componentExports("functions/reentrance", {
    cb(n) {
      if (n === 0) throw new Error("stop");
      return exports.outer(n - 1);
    },
  });
  assert_traps(() => exports.outer(2));
}, "an exception thrown inside a re-entrant chain surfaces as a trap");
