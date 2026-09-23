// META: global=window,dedicatedworker,jsshell
// META: script=/wasm/component/jsapi/harness/component-helpers.js
// META: script=/wasm/component/jsapi/harness/assertions.js

// Compound types nested inside each other. See components/values/nesting.wat
// for the type definitions.

let exports;
let received;
promise_setup(async () => {
  const echo = x => { received = x; return x; };
  exports = await componentExports("values/nesting", {
    echoListRecord: echo, echoOptionList: echo, echoShape: echo, echoTuple: echo,
    echoResults: echo, echoDeep: echo, echoWebPage: echo,
  });
});

function check(name, input, expected, description) {
  promise_test(async () => {
    const result = exports[name](input);
    assert_value_equals(received, expected, "value seen by echo");
    assert_value_equals(result, expected);
  }, description);
}

check("roundtripListRecord",
      [{ name: "a", tags: ["x", "y"] }, { name: "b", tags: [] }],
      [record({ name: "a", tags: ["x", "y"] }), record({ name: "b", tags: [] })],
      "list<record { name: string, tags: list<string> }>");
check("roundtripListRecord", [], [], "empty list<record>");
check("roundtripListRecord", new Set([{ name: "s", tags: new Set(["t"]) }]),
      [record({ name: "s", tags: ["t"] })], "list<record> from nested iterables");

check("roundtripOptionList", null, null, "option<list<option<u32>>>: none");
check("roundtripOptionList", [], [], "option<list<option<u32>>>: some(empty)");
check("roundtripOptionList", [null, 1, undefined, 0], [null, 1, null, 0],
      "option<list<option<u32>>>: inner none and some");

check("roundtripShape", { kind: "Point", value: { x: 1, y: 2 } }, variant("Point", record({ x: 1, y: 2 })),
      "variant with a record payload");
check("roundtripShape", { kind: "Name", value: "n" }, variant("Name", "n"), "variant with a string payload");

check("roundtripTuple", ["s", new Uint8Array([1, 2, 3]), "c"], ["s", new Uint8Array([1, 2, 3]), "c"],
      "tuple<string, list<u8>, option<char>>");
check("roundtripTuple", ["", [], null], ["", new Uint8Array(0), null],
      "tuple<string, list<u8>, option<char>>: empty and none");
check("roundtripTuple", ["😀", [255], "😀"], ["😀", new Uint8Array([255]), "😀"],
      "tuple<string, list<u8>, option<char>>: astral char and max byte");

check("roundtripResults",
      [{ kind: "Ok", value: { x: 1, y: 2 } }, { kind: "Error", value: ["a", "b"] }],
      [variant("Ok", record({ x: 1, y: 2 })), variant("Error", ["a", "b"])],
      "list<result<record, list<string>>>");

check("roundtripWebPage", { url: "https://example.com", httpStatus: 200, xmlBody: "<a/>" },
      record({ url: "https://example.com", httpStatus: 200, xmlBody: "<a/>" }),
      "record { URL, HTTP-status, XML-body } uses camelCase keys url, httpStatus, xmlBody");
check("roundtripWebPage", { url: "u", httpStatus: 404 },
      record({ url: "u", httpStatus: 404, xmlBody: null }),
      "record with acronym labels: missing option field");

promise_test(async () => {
  const result = exports.roundtripWebPage({ url: "u", httpStatus: 1, xmlBody: null, URL: "wrong", HTTPStatus: 2 });
  assert_array_equals(Reflect.ownKeys(result), ["url", "httpStatus", "xmlBody"]);
  assert_equals(result.url, "u");
  assert_equals(result.httpStatus, 1);
}, "record with acronym labels: only the camelCase keys are read");

function nest(depth, leaf) {
  let value = leaf;
  for (let i = 1; i < depth; i++) {
    value = [value];
  }
  return value;
}

promise_test(async () => {
  const input = nest(50, [1, 2, 3]);
  const result = exports.roundtripDeep(input);
  assert_value_equals(received, input, "value seen by echo");
  assert_value_equals(result, input);
}, "50-deep list<list<...<u32>>> roundtrips");

promise_test(async () => {
  assert_value_equals(exports.roundtripDeep([]), []);
  assert_value_equals(exports.roundtripDeep(nest(25, [])), nest(25, []));
  assert_value_equals(exports.roundtripDeep([nest(49, [7]), nest(49, [])]), [nest(49, [7]), nest(49, [])]);
}, "50-deep list: empty lists at various depths");

// The failing element is converted after the outer lists are allocated in the
// guest, which locks down the instance, so each case uses a fresh one.
for (const [input, description] of [
  [() => nest(49, [1]), "one level too shallow"],
  [() => nest(50, "x"), "wrong leaf type"],
]) {
  promise_test(async () => {
    const { roundtripDeep } = await componentExports("values/nesting", {
      echoListRecord() {}, echoOptionList() {}, echoShape() {}, echoTuple() {},
      echoResults() {}, echoDeep: x => x, echoWebPage() {},
    });
    assert_throws_js(TypeError, () => roundtripDeep(input()));
  }, `50-deep list: ${description} throws TypeError`);
}

promise_test(async () => {
  const { roundtripShape } = await componentExports("values/nesting", {
    echoListRecord: x => x, echoOptionList: x => x, echoShape: () => ({ kind: "Point", value: { x: 1 } }),
    echoTuple: x => x, echoResults: x => x, echoDeep: x => x, echoWebPage: x => x,
  });
  assert_traps(() => roundtripShape({ kind: "Name", value: "n" }));
}, "nested conversion failure in an import return value traps");
