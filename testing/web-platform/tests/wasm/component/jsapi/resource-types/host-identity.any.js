// META: global=window,dedicatedworker,jsshell
// META: script=/wasm/component/jsapi/harness/component-helpers.js
// META: script=/wasm/component/jsapi/harness/assertions.js

// A host resource value is the original JS object, so it comes back `===`
// wherever it flows: params, results, import arguments and containers.

const FIXTURE = "resources/host-identity";

class C {}

let received;
let exports;
promise_setup(async () => {
  exports = await componentExports(FIXTURE, { R: C, recv: x => { received = x; } });
});

promise_test(async () => {
  const c = new C();
  received = undefined;
  exports.send(c);
  assert_equals(received, c);
}, "a borrow param forwarded to an import arrives as the same object");

promise_test(async () => {
  const c = new C();
  exports.store(c);
  assert_equals(exports.load(), c);
}, "an own handle kept by the guest lifts back to the same object");

promise_test(async () => {
  const a = new C();
  const b = new C();
  const result = exports.roundtripList([a, b, a]);
  assert_true(Array.isArray(result));
  assert_equals(result.length, 3);
  assert_equals(result[0], a);
  assert_equals(result[1], b);
  assert_equals(result[2], a);
}, "list<own<r>>: elements are the same objects, including repeats");

promise_test(async () => {
  assert_value_equals(exports.roundtripList([]), []);
}, "list<own<r>>: empty");

// Elements are converted after the list is allocated in the guest, so a failing
// element locks down the instance, and this test uses fresh ones.
promise_test(async () => {
  class Other {}
  const a = new C();
  for (const list of [[a, new Other()], [1]]) {
    const fresh = await componentExports(FIXTURE, { R: C, recv: () => {} });
    assert_throws_js(TypeError, () => fresh.roundtripList(list));
  }
}, "list<own<r>>: an element failing the brand check is a TypeError");

promise_test(async () => {
  const c = new C();
  assert_equals(exports.roundtripOption(c), c, "some");
  assert_equals(exports.roundtripOption(null), undefined, "none from null");
  assert_equals(exports.roundtripOption(undefined), undefined, "none from undefined");
}, "option<own<r>>");

promise_test(async () => {
  const c = new C();
  const result = exports.roundtripRecord({ a: c, b: 7 });
  assert_value_equals(result, record({ a: c, b: 7 }));
  assert_equals(result.a, c);
}, "record field of type own<r>");

promise_test(async () => {
  const c = new C();
  const some = exports.roundtripVariant({ kind: "some", value: c });
  assert_value_equals(some, variant("some", c));
  assert_equals(some.value, c);
  assert_value_equals(exports.roundtripVariant({ kind: "none" }), variant("none"));
}, "variant payload of type own<r>");

promise_test(async () => {
  const a = new C();
  const b = new C();
  const result = exports.roundtripMap(new Map([["a", a], ["b", b]]));
  assert_value_equals(result, new Map([["a", a], ["b", b]]));
  assert_equals(result.get("a"), a);
  assert_equals(result.get("b"), b);
}, "map<string, own<r>> values");

promise_test(async () => {
  const c = new C();
  const error = assert_component_error(() => exports.fail(c), c);
  assert_equals(error.data, c);
}, "result error payload of type own<r> is the same object on ComponentError.data");

promise_test(async () => {
  const c = new C();
  assert_equals(exports.roundtripOption(c), c, "first");
  assert_equals(exports.roundtripOption(c), c, "second");
  exports.send(c);
  assert_equals(received, c, "as borrow after two owns");
}, "the same object can be converted repeatedly; handles are not canonicalized");

promise_test(async () => {
  const url = new URL("https://example.com/");
  const { roundtripOption, send } = await componentExports(FIXTURE, { R: URL, recv: x => { received = x; } });
  assert_equals(roundtripOption(url), url);
  send(url);
  assert_equals(received, url);
}, "platform objects are host resource values too");
