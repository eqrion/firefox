// META: global=window,dedicatedworker,jsshell
// META: script=/common/gc.js
// META: script=/wasm/component/jsapi/harness/component-helpers.js
// META: script=/wasm/component/jsapi/harness/assertions.js

// Guest resource instances that become unreachable are dropped through the
// guest resource FinalizationRegistry. Collection is not guaranteed, so these
// tests poll and give up rather than fail outright when no collection happens.

async function waitForDropCount(t, dropCount, expected) {
  for (let i = 0; i < 40; i++) {
    await garbageCollect();
    if (dropCount() >= expected) {
      return;
    }
    await new Promise(resolve => t.step_timeout(resolve, 25));
  }
}

function createAndForget(Counter, n) {
  for (let i = 0; i < n; i++) {
    new Counter(i);
  }
}

promise_test(async t => {
  const { Counter, dropCount } = await componentExports("gc/counter");
  createAndForget(Counter, 10);
  await waitForDropCount(t, dropCount, 10);
  assert_equals(dropCount(), 10);
}, "unreachable owned instances are eventually dropped");

promise_test(async t => {
  const { Counter, dropCount } = await componentExports("gc/counter");
  createAndForget(Counter, 5);
  const kept = new Counter(99);
  await waitForDropCount(t, dropCount, 5);
  assert_equals(dropCount(), 5);
  assert_equals(kept.get(), 99, "a reachable instance is still alive");
  kept[Symbol.dispose]();
  assert_equals(dropCount(), 6);
}, "reachable instances survive collection");

promise_test(async t => {
  const { Counter, dropCount } = await componentExports("gc/counter");
  (() => {
    for (let i = 0; i < 5; i++) {
      new Counter(i)[Symbol.dispose]();
    }
  })();
  assert_equals(dropCount(), 5);
  await waitForDropCount(t, dropCount, 6);
  assert_equals(dropCount(), 5, "disposed instances are unregistered and not dropped again");
}, "disposed instances are not dropped a second time by the collector");

promise_test(async t => {
  const { Counter, take, dropCount } = await componentExports("gc/counter");
  (() => {
    for (let i = 0; i < 5; i++) {
      take(new Counter(i));
    }
  })();
  assert_equals(dropCount(), 5, "the guest dropped the transferred handles");
  await waitForDropCount(t, dropCount, 6);
  assert_equals(dropCount(), 5, "transferred instances are unregistered and not dropped again");
}, "instances transferred as own are not dropped a second time by the collector");
