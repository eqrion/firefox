// META: global=window,dedicatedworker,jsshell
// META: script=/wasm/component/jsapi/harness/component-helpers.js
// META: script=/wasm/component/jsapi/harness/assertions.js

// Guest resources nested inside container types, in both directions.

let exports;
promise_setup(async () => {
  exports = await componentExports("resources/guest-containers");
});

promise_test(async () => {
  const { Counter, wrapList, unwrapList, dropCount } = exports;
  const list = wrapList(3);
  assert_true(Array.isArray(list));
  assert_equals(list.length, 3);
  for (let i = 0; i < 3; i++) {
    assert_true(list[i] instanceof Counter, `element ${i}`);
    assert_equals(list[i].value(), i, `element ${i} value`);
  }
  assert_equals(new Set(list).size, 3, "distinct instances");
  const before = dropCount();
  assert_equals(unwrapList(list), 3);
  assert_equals(dropCount(), before + 3);
  assert_throws_js(TypeError, () => list[0].value(), "elements were transferred");
}, "list<own<R>>");

promise_test(async () => {
  const { wrapList, unwrapList } = exports;
  assert_array_equals(wrapList(0), []);
  assert_equals(unwrapList([]), 0);
  assert_equals(unwrapList(new Set(wrapList(2))), 1, "any iterable works");
}, "list<own<R>>: empty and iterable inputs");

promise_test(async () => {
  const { Counter, wrapOption, unwrapOption } = exports;
  const some = wrapOption(true);
  assert_true(some instanceof Counter);
  assert_equals(some.value(), 5);
  assert_equals(wrapOption(false), undefined);
  assert_equals(unwrapOption(null), 0xffff);
  assert_equals(unwrapOption(undefined), 0xffff);
  assert_equals(unwrapOption(some), 5);
  assert_throws_js(TypeError, () => some.value(), "transferred");
}, "option<own<R>>");

promise_test(async () => {
  const { Counter, wrapResult, take, dropCount } = exports;
  const ok = wrapResult(true);
  assert_true(ok instanceof Counter);
  assert_equals(ok.value(), 1);
  const before = dropCount();
  take(ok);
  assert_equals(dropCount(), before + 1, "the lifted ok payload can be passed back as own");
}, "result<own<R>, _> ok payload");

promise_test(async () => {
  const { Counter, wrapResult, take, dropCount } = exports;
  let thrown;
  try {
    wrapResult(false);
  } catch (e) {
    thrown = e;
  }
  assert_equals(Object.getPrototypeOf(thrown), WebAssembly.ComponentError.prototype);
  assert_true(thrown.data instanceof Counter, "data is a guest instance");
  assert_equals(thrown.data.value(), 2);
  const before = dropCount();
  take(thrown.data);
  assert_equals(dropCount(), before + 1, "the error payload can be passed back as own");
}, "result<_, own<R>> error payload throws ComponentError carrying the instance");

promise_test(async () => {
  const { Counter, unwrapResult, dropCount } = exports;
  const counter = exports.make();
  const before = dropCount();
  assert_equals(unwrapResult(variant("ok", counter)), 0);
  assert_equals(dropCount(), before + 1);
  assert_equals(unwrapResult(variant("error", 5)), 1005);
  assert_throws_js(TypeError, () => unwrapResult(variant("Error", 5)), "case labels are ok and error");
  assert_throws_js(TypeError, () => unwrapResult(exports.make()), "a result parameter is not unwrapped");
}, "result<own<R>, u32> as a parameter uses the variant shape");

promise_test(async () => {
  const { Counter, wrapRecord, unwrapRecord, dropCount } = exports;
  const rec = wrapRecord();
  assert_equals(Object.getPrototypeOf(rec), null);
  assert_array_equals(Object.keys(rec), ["c", "n"]);
  assert_true(rec.c instanceof Counter);
  assert_equals(rec.c.value(), 3);
  assert_equals(rec.n, 7);
  const before = dropCount();
  assert_equals(unwrapRecord(rec), 10);
  assert_equals(dropCount(), before + 1);
  assert_equals(unwrapRecord({ c: exports.make(), n: 1 }), 1, "plain objects work");
}, "record with an own<R> field");

promise_test(async () => {
  const { Counter, wrapVariant, unwrapVariant, dropCount } = exports;
  const full = wrapVariant(true);
  assert_equals(Object.getPrototypeOf(full), null);
  assert_equals(full.kind, "full");
  assert_true(full.value instanceof Counter);
  assert_equals(full.value.value(), 9);
  const empty = wrapVariant(false);
  assert_equals(empty.kind, "empty");
  assert_false("value" in empty);
  const before = dropCount();
  assert_equals(unwrapVariant(full), 9);
  assert_equals(dropCount(), before + 1);
  assert_equals(unwrapVariant(empty), 0xffff);
  assert_equals(unwrapVariant({ kind: "full", value: exports.make() }), 0);
}, "variant with an own<R> payload");

promise_test(async () => {
  const { Counter, wrapMap, unwrapMap, dropCount } = exports;
  const map = wrapMap();
  assert_equals(Object.getPrototypeOf(map), Map.prototype);
  assert_array_equals([...map.keys()], ["a", "b"]);
  assert_true(map.get("a") instanceof Counter);
  assert_equals(map.get("a").value(), 1);
  assert_equals(map.get("b").value(), 2);
  const before = dropCount();
  assert_equals(unwrapMap(map), 3);
  assert_equals(dropCount(), before + 2);
  assert_equals(unwrapMap([["x", exports.make()]]), 0, "iterable of pairs");
  assert_equals(unwrapMap({ x: exports.make() }), 0, "plain object with string keys");
}, "map<string, own<R>>");

// Elements are converted after the list is allocated in the guest, so a failing
// element locks down the instance, and this test uses a fresh one. Disposing a
// consumed instance does nothing, even though the instance is locked down.
promise_test(async () => {
  const { make, unwrapList, dropCount } = await componentExports("resources/guest-containers");
  const a = make();
  assert_throws_js(TypeError, () => unwrapList([a, 1]));
  assert_traps(() => dropCount(), "the instance is locked down");
  assert_equals(a[Symbol.dispose](), undefined, "earlier owns are consumed");
}, "a container with an invalid element fails after consuming earlier owns");
