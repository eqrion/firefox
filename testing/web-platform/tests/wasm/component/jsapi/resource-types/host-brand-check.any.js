// META: global=window,dedicatedworker,jsshell
// META: script=/wasm/component/jsapi/harness/component-helpers.js
// META: script=/wasm/component/jsapi/harness/assertions.js

// Brand checks for host (imported) resource types. The fixture exports
// identity(x: own<r>) -> own<r>, checkBorrow(x: borrow<r>) -> u32 and
// makeViaImport() -> own<r>, which lifts the return value of the `make` import.

const FIXTURE = "resources/host-brand-check";

async function instantiate(ctor, make = () => { throw new Error("unused"); }) {
  return componentExports(FIXTURE, { R: ctor, make });
}

for (const value of [undefined, null, true, 1, "URL", 1n, Symbol(), {}, [], URL.prototype]) {
  test_instantiation_error(WebAssembly.LinkError, FIXTURE, { R: value, make() {} }, undefined,
                           `a resource type import satisfied by ${format_value(value)} is a LinkError`);
}

promise_test(async () => {
  class C {}
  class Sub extends C {}
  const { identity, checkBorrow } = await instantiate(C);
  const c = new C();
  const sub = new Sub();
  assert_equals(identity(c), c, "own: instance of the class");
  assert_equals(identity(sub), sub, "own: instance of a subclass");
  assert_equals(checkBorrow(c), 1, "borrow: instance of the class");
  assert_equals(checkBorrow(sub), 1, "borrow: instance of a subclass");
}, "plain class: instances and subclass instances pass the instanceof check");

promise_test(async () => {
  class C {}
  const { identity, checkBorrow } = await instantiate(C);
  for (const value of [undefined, null, true, 1, "s", 1n, Symbol()]) {
    assert_throws_js(TypeError, () => identity(value), `own: ${format_value(value)}`);
    assert_throws_js(TypeError, () => checkBorrow(value), `borrow: ${format_value(value)}`);
  }
}, "plain class: primitives are a TypeError");

promise_test(async () => {
  class C {}
  class Other {}
  const { identity, checkBorrow } = await instantiate(C);
  for (const value of [{}, [], new Other(), Object.create(null), () => {}, C, C.prototype]) {
    assert_throws_js(TypeError, () => identity(value), `own: ${format_value(value)}`);
    assert_throws_js(TypeError, () => checkBorrow(value), `borrow: ${format_value(value)}`);
  }
}, "plain class: unrelated objects are a TypeError");

promise_test(async () => {
  class C {}
  const { identity } = await instantiate(C);
  const fake = Object.create(C.prototype);
  assert_equals(identity(fake), fake);
}, "plain class: an object created from the prototype passes, since only instanceof is checked");

promise_test(async () => {
  const marker = {};
  class H {
    static [Symbol.hasInstance](value) { return value === marker; }
  }
  const { identity } = await instantiate(H);
  assert_equals(identity(marker), marker, "value accepted by Symbol.hasInstance");
  assert_throws_js(TypeError, () => identity(new H()), "value rejected by Symbol.hasInstance");
}, "plain class: Symbol.hasInstance is honoured");

promise_test(async t => {
  class C {}
  const { identity } = await instantiate(C);
  const value = {};
  assert_throws_js(TypeError, () => identity(value), "before patching");
  Object.defineProperty(C, Symbol.hasInstance, { value: () => true, configurable: true });
  t.add_cleanup(() => delete C[Symbol.hasInstance]);
  assert_equals(identity(value), value, "after patching");
}, "plain class: the instanceof check is performed at each conversion, so a later Symbol.hasInstance is seen");

promise_test(async () => {
  const arrow = () => {};
  const { identity, checkBorrow } = await instantiate(arrow);
  assert_throws_js(TypeError, () => identity({}), "own: object");
  assert_throws_js(TypeError, () => checkBorrow({}), "borrow: object");
  assert_throws_js(TypeError, () => identity(1), "own: primitive");
}, "arrow function: linking succeeds but every conversion is a TypeError, since it has no prototype");

promise_test(async () => {
  const { identity, checkBorrow } = await instantiate(URL, () => new URL("https://example.com/"));
  const url = new URL("https://example.com/");
  assert_equals(identity(url), url, "own");
  assert_equals(checkBorrow(url), 1, "borrow");
}, "WebIDL interface object: a platform object implementing the interface passes");

promise_test(async () => {
  class MyURL extends URL {}
  const { identity } = await instantiate(URL);
  const url = new MyURL("https://example.com/");
  assert_equals(identity(url), url);
}, "WebIDL interface object: a subclass instance passes");

promise_test(async () => {
  const { identity, checkBorrow } = await instantiate(URL);
  const fake = Object.create(URL.prototype);
  assert_throws_js(TypeError, () => identity(fake), "own");
  assert_throws_js(TypeError, () => checkBorrow(fake), "borrow");
  assert_throws_js(TypeError, () => identity({ href: "https://example.com/" }), "own: duck");
  assert_throws_js(TypeError, () => identity("https://example.com/"), "own: string");
}, "WebIDL interface object: objects that do not implement the interface are a TypeError");

promise_test(async t => {
  const { identity } = await instantiate(URL);
  Object.defineProperty(URL, Symbol.hasInstance, { value: () => true, configurable: true });
  t.add_cleanup(() => delete URL[Symbol.hasInstance]);
  const value = {};
  assert_true(value instanceof URL, "instanceof itself is patched");
  assert_throws_js(TypeError, () => identity(value));
}, "WebIDL interface object: Symbol.hasInstance is ignored");

promise_test(async () => {
  const source = await componentExports("resources/host-guest-source");
  const { identity, checkBorrow } = await instantiate(source.Counter, () => new source.Counter(1));
  const counter = new source.Counter(5);
  assert_equals(identity(counter), counter, "own");
  assert_equals(checkBorrow(counter), 1, "borrow");
}, "guest resource class: instances pass");

promise_test(async () => {
  const source = await componentExports("resources/host-guest-source");
  const { identity, checkBorrow } = await instantiate(source.Counter);
  const fake = Object.create(source.Counter.prototype);
  assert_throws_js(TypeError, () => identity(fake), "own");
  assert_throws_js(TypeError, () => checkBorrow(fake), "borrow");
  class Sub extends source.Counter {}
  const sub = new Sub(1);
  assert_equals(identity(sub), sub, "a subclass instance still has the [[ResourceClass]] slot");
}, "guest resource class: the [[ResourceClass]] slot is checked, not the prototype chain");

promise_test(async () => {
  const sourceA = await componentExports("resources/host-guest-source");
  const sourceB = await componentExports("resources/host-guest-source");
  const { identity } = await instantiate(sourceA.Counter);
  assert_throws_js(TypeError, () => identity(new sourceB.Counter(1)));
}, "guest resource class: an instance of the same class from another instantiation is a TypeError");

promise_test(async t => {
  const source = await componentExports("resources/host-guest-source");
  const { identity } = await instantiate(source.Counter);
  Object.defineProperty(source.Counter, Symbol.hasInstance, { value: () => true, configurable: true });
  t.add_cleanup(() => delete source.Counter[Symbol.hasInstance]);
  assert_throws_js(TypeError, () => identity({}));
}, "guest resource class: Symbol.hasInstance is ignored");

promise_test(async () => {
  const source = await componentExports("resources/host-guest-source");
  const { identity } = await instantiate(source.Counter);
  const counter = new source.Counter(1);
  assert_equals(source.take(counter), 1, "take consumes the instance");
  assert_equals(identity(counter), counter);
}, "guest resource class: a dropped instance still passes because only the class is checked");

promise_test(async () => {
  class T {
    static [Symbol.hasInstance]() { throw new RangeError("boom"); }
  }
  const { identity, checkBorrow } = await instantiate(T);
  assert_throws_js(RangeError, () => identity({}), "own");
  assert_throws_js(RangeError, () => checkBorrow({}), "borrow");
}, "a throwing Symbol.hasInstance propagates its exception from an argument conversion");

promise_test(async () => {
  class C {}
  const { identity } = await instantiate(C);
  const value = new Proxy({}, { getPrototypeOf() { throw new RangeError("boom"); } });
  assert_throws_js(RangeError, () => identity(value));
}, "a throwing [[GetPrototypeOf]] propagates its exception from an argument conversion");

promise_test(async () => {
  class T {
    static [Symbol.hasInstance]() { throw new RangeError("boom"); }
  }
  const { makeViaImport } = await instantiate(T, () => ({}));
  assert_traps(() => makeViaImport());
}, "a throwing brand check while converting an import's return value traps");

promise_test(async () => {
  class C {}
  const { makeViaImport } = await instantiate(C, () => ({}));
  assert_traps(() => makeViaImport());
}, "an import returning a value that fails the brand check traps");

promise_test(async () => {
  class C {}
  const c = new C();
  const { makeViaImport } = await instantiate(C, () => c);
  assert_equals(makeViaImport(), c);
}, "an import returning a value that passes the brand check lifts to the same object");
