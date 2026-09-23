// META: global=window,dedicatedworker,jsshell
// META: script=/wasm/component/jsapi/harness/component-helpers.js
// META: script=/wasm/component/jsapi/harness/assertions.js

// One host resource type is created per abstract type import, so `r1` and
// `r2` are distinct even when the same constructor satisfies both, while the
// transparent `(import "r3" (type (eq $r1)))` shares r1's type.

const FIXTURE = "resources/host-distinct-types";

class C {}

promise_test(async () => {
  let called = false;
  const { forward } = await componentExports(FIXTURE, { r1: C, r2: C, r3: C, "use-r2": () => { called = true; } });
  assert_traps(() => forward(new C()));
  assert_false(called, "the import must not run");
}, "an r1 handle passed by the guest where r2 is expected traps");

promise_test(async () => {
  const { r2Identity } = await componentExports(FIXTURE, { r1: C, r2: C, r3: C, "use-r2": () => {} });
  const c = new C();
  assert_equals(r2Identity(c), c);
}, "r2 works on its own with the shared constructor");

promise_test(async () => {
  class D {}
  const { forward, r2Identity, r3ToR1 } = await componentExports(FIXTURE, { r1: C, r2: D, r3: C, "use-r2": () => {} });
  assert_throws_js(TypeError, () => forward(new D()), "D where r1 expected");
  assert_throws_js(TypeError, () => r2Identity(new C()), "C where r2 expected");
  assert_throws_js(TypeError, () => r3ToR1(new D()), "D where r3 (= r1) expected");
}, "each abstract import brand checks against its own constructor");

promise_test(async () => {
  const { r3ToR1, r1ToR3 } = await componentExports(FIXTURE, { r1: C, r2: C, r3: undefined, "use-r2": () => {} });
  const c = new C();
  assert_equals(r3ToR1(c), c);
  assert_equals(r1ToR3(c), c);
}, "a transparent (eq) import shares the abstract import's type in both directions");

for (const value of [undefined, null, 42, "x", {}, class Unrelated {}]) {
  promise_test(async () => {
    const log = [];
    const imports = loggingProxy({ r1: C, r2: C, r3: value, "use-r2": () => {} }, log);
    const { r3ToR1 } = await componentExports(FIXTURE, imports);
    assert_array_equals(log, ["get r1", "get r2", "get use-r2"], "the transparent import is skipped");
    const c = new C();
    assert_equals(r3ToR1(c), c, "conversions use r1's constructor");
  }, `a transparent (eq) import is not read from the imports object, so ${format_value(value)} is fine`);
}
