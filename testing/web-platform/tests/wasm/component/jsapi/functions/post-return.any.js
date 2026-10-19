// META: global=window,dedicatedworker,jsshell
// META: script=/wasm/component/jsapi/harness/component-helpers.js
// META: script=/wasm/component/jsapi/harness/assertions.js

promise_test(async () => {
  const { get, postRuns, postArg } = await componentExports("functions/post-return", {
    notify: () => {},
  });
  assert_equals(postRuns(), 0);
  assert_equals(get(), 0, "the lifted value predates post-return");
  assert_equals(postRuns(), 1, "post-return ran before get returned to JS");
  assert_equals(postArg(), 0, "post-return received the returned flat value");
  assert_equals(get(), 1);
  assert_equals(postRuns(), 2);
  assert_equals(postArg(), 1);
}, "post-return runs after lifting and before the caller sees the result");

promise_test(async () => {
  const notified = [];
  const { getNotifying, postRuns } = await componentExports("functions/post-return", {
    notify: v => notified.push(v),
  });
  assert_traps(() => getNotifying());
  assert_array_equals(notified, [], "the import is not called");
  assert_traps(() => postRuns(), "instance is locked down");
}, "calling an import from post-return traps");
