// META: global=window,dedicatedworker,jsshell
// META: script=/wasm/component/jsapi/harness/component-helpers.js
// META: script=/wasm/component/jsapi/harness/assertions.js

promise_test(async () => {
  const notified = [];
  const { get, postRuns } = await componentExports("functions/post-return", {
    notify: v => notified.push(v),
  });
  assert_equals(postRuns(), 0);
  assert_equals(get(), 0, "the lifted value predates post-return");
  assert_equals(postRuns(), 1, "post-return ran before get returned to JS");
  assert_array_equals(notified, [0], "post-return received the returned flat value");
  assert_equals(get(), 1);
  assert_equals(postRuns(), 2);
  assert_array_equals(notified, [0, 1]);
}, "post-return runs after lifting and before the caller sees the result");

promise_test(async () => {
  let order = [];
  const { get } = await componentExports("functions/post-return", {
    notify: () => order.push("post-return"),
  });
  const value = get();
  order.push(`returned ${value}`);
  assert_array_equals(order, ["post-return", "returned 0"]);
}, "an import called from post-return runs before the export returns");

promise_test(async () => {
  const { get, postRuns } = await componentExports("functions/post-return", {
    notify: () => { throw new Error("in post-return"); },
  });
  assert_traps(() => get());
  assert_traps(() => postRuns(), "instance is locked down");
}, "an import throwing during post-return traps");
