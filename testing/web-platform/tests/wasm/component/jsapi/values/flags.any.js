// META: global=window,dedicatedworker,jsshell
// META: script=/wasm/component/jsapi/harness/component-helpers.js
// META: script=/wasm/component/jsapi/harness/assertions.js

// flags of 1, 8, 16 and 32 labels. The eight-label type is
// flags { read, write, exec, is-dir, is-file, hidden, read-only, HTML }.

const eightLabels = ["read", "write", "exec", "isDir", "isFile", "hidden", "readOnly", "html"];
const sixteenLabels = Array.from({ length: 16 }, (_, i) => `b${i}`);
const thirtyTwoLabels = Array.from({ length: 32 }, (_, i) => `b${i}`);

function allFlags(labels, value) {
  return flags(Object.fromEntries(labels.map(l => [l, value])));
}

let exports;
let received;
promise_setup(async () => {
  const echo = x => { received = x; return x; };
  exports = await componentExports("values/flags", {
    echoOne: echo, echoEight: echo, echoSixteen: echo, echoThirtyTwo: echo,
  });
});

promise_test(async () => {
  const result = exports.roundtripEight({ read: true, isDir: true });
  const expected = flags({ ...Object.fromEntries(eightLabels.map(l => [l, false])), read: true, isDir: true });
  assert_value_equals(received, expected, "value seen by echo");
  assert_value_equals(result, expected);
  assert_equals(Object.getPrototypeOf(result), null);
  assert_array_equals(Reflect.ownKeys(result), eightLabels);
}, "flags: result is a null-prototype object with every camelCase label as a Boolean");

promise_test(async () => {
  const result = exports.roundtripEight({});
  assert_value_equals(result, allFlags(eightLabels, false));
}, "flags: absent labels are false");

promise_test(async () => {
  const result = exports.roundtripEight({
    read: "yes", write: 0, exec: "", isDir: [], isFile: null, hidden: {}, readOnly: NaN, html: 1n,
  });
  assert_value_equals(result, flags({
    read: true, write: false, exec: false, isDir: true, isFile: false, hidden: true, readOnly: false, html: true,
  }));
}, "flags: labels are converted with ToBoolean");

promise_test(async () => {
  const log = [];
  exports.roundtripEight(loggingProxy({}, log));
  assert_array_equals(log, eightLabels.map(l => `get ${l}`));
}, "flags: labels are read with Get in declaration order");

promise_test(async () => {
  const result = exports.roundtripEight({ read: true, extra: true, Read: true, "is-dir": true });
  assert_value_equals(result, flags({ ...Object.fromEntries(eightLabels.map(l => [l, false])), read: true }));
}, "flags: unknown, kebab and mis-cased properties are ignored");

promise_test(async () => {
  const result = exports.roundtripEight(Object.create({ hidden: true }));
  assert_true(result.hidden);
}, "flags: inherited properties are honoured");

for (const value of [null, undefined, "read", 1, true, Symbol()]) {
  promise_test(async () => {
    received = "unchanged";
    assert_throws_js(TypeError, () => exports.roundtripEight(value));
    assert_equals(received, "unchanged", "echo must not be called");
  }, `flags: non-object ${format_value(value)} throws TypeError`);
}

promise_test(async () => {
  assert_value_equals(exports.roundtripOne({}), flags({ a: false }));
  assert_value_equals(exports.roundtripOne({ a: true }), flags({ a: true }));
  assert_array_equals(Reflect.ownKeys(exports.roundtripOne({})), ["a"]);
}, "flags: one label");

for (const [name, labels] of [["roundtripSixteen", sixteenLabels], ["roundtripThirtyTwo", thirtyTwoLabels]]) {
  promise_test(async () => {
    assert_value_equals(exports[name]({}), allFlags(labels, false), "none set");
    assert_value_equals(exports[name](allFlags(labels, true)), allFlags(labels, true), "all set");
    const alternating = flags(Object.fromEntries(labels.map((l, i) => [l, i % 2 === 0])));
    assert_value_equals(exports[name](alternating), alternating, "alternating");
    const last = { [labels[labels.length - 1]]: true };
    assert_value_equals(exports[name](last), flags({ ...Object.fromEntries(labels.map(l => [l, false])), ...last }),
                        "only the last label");
    assert_array_equals(Reflect.ownKeys(exports[name]({})), labels, "key order");
  }, `flags: ${labels.length} labels`);
}

promise_test(async () => {
  const { roundtripEight } = await componentExports("values/flags", {
    echoOne: x => x, echoEight: () => ({ exec: 1 }), echoSixteen: x => x, echoThirtyTwo: x => x,
  });
  assert_value_equals(roundtripEight({}), flags({ ...Object.fromEntries(eightLabels.map(l => [l, false])), exec: true }));
}, "flags: an import return value is converted with the same rules");

promise_test(async () => {
  const { roundtripEight } = await componentExports("values/flags", {
    echoOne: x => x, echoEight: () => null, echoSixteen: x => x, echoThirtyTwo: x => x,
  });
  assert_traps(() => roundtripEight({}));
}, "flags: an import returning a non-object traps");
