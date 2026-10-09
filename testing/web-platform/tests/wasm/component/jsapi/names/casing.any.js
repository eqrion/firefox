// META: global=window,dedicatedworker,jsshell
// META: script=/wasm/component/jsapi/harness/component-helpers.js
// META: script=/wasm/component/jsapi/harness/assertions.js

// [label, CamelCase, PascalCase] for every row of the spec's casing table.
const casingTable = [
  ["element", "element", "Element"],
  ["query-selector", "querySelector", "QuerySelector"],
  ["inner-HTML", "innerHTML", "InnerHTML"],
  ["XML-http-request", "xmlHttpRequest", "XMLHttpRequest"],
  ["URL", "url", "URL"],
  ["a1-2-3", "a123", "A123"],
  ["HTTP-request", "httpRequest", "HTTPRequest"],
];
const labels = casingTable.map(([label]) => label);
const camelNames = casingTable.map(([, camel]) => camel);
const pascalNames = casingTable.map(([, , pascal]) => pascal);

promise_test(async () => {
  const log = [];
  const imports = {};
  camelNames.forEach((name, i) => { imports[name] = () => i; });
  const exports = await componentExports("names/casing-funcs", loggingProxy(imports, log));
  assert_array_equals(log, camelNames.map(name => `get ${name}`), "import lookup keys");
  assert_array_equals(Reflect.ownKeys(exports), camelNames, "export keys");
  camelNames.forEach((name, i) => {
    assert_equals(exports[name](), i, `${name} calls the import read under the same key`);
  });
}, "function imports and exports are named with CamelCase");

promise_test(async () => {
  const imports = {};
  for (const [label, camel] of casingTable) {
    imports[label] = () => { throw new Error(`kebab key ${label} must not be read`); };
    imports[camel] = () => 0;
  }
  const exports = await componentExports("names/casing-funcs", imports);
  for (const [label, camel] of casingTable) {
    if (label !== camel) {
      assert_false(Object.hasOwn(exports, label), `export key ${label} is not kebab-case`);
    }
  }
}, "kebab-case names are not used as JS keys");

promise_test(async () => {
  const exports = await componentExports("names/casing-types");
  assert_array_equals(Reflect.ownKeys(exports), pascalNames, "export keys");
  for (const name of pascalNames) {
    assert_equals(typeof exports[name], "function", `${name} is a class`);
    assert_function_name(exports[name], name, name);
  }
}, "resource type exports are named with PascalCase");

promise_test(async () => {
  const { Element } = await componentExports("names/casing-types");
  assert_guest_class(Element, {
    name: "Element",
    length: 0,
    methods: [["querySelector", 0], ["innerHTML", 0], ["a123", 0], ["httpRequest", 0]],
    statics: [["xmlHttpRequest", 0], ["url", 0]],
  });
  const element = new Element();
  assert_equals(element.querySelector(), 7);
  assert_equals(element.innerHTML(), 7);
  assert_equals(element.a123(), 7);
  assert_equals(element.httpRequest(), 7);
  assert_equals(Element.xmlHttpRequest(), 1);
  assert_equals(Element.url(), 1);
}, "methods and statics are named with CamelCase");

let values;
promise_setup(async () => {
  values = await componentExports("names/casing-values");
});

promise_test(async () => {
  const expectedFuncKeys = ["roundtripRecord", "roundtripFlags", "roundtripVariant", "roundtripEnum"];
  assert_array_equals(Reflect.ownKeys(values).filter(k => String(k).startsWith("roundtrip")),
                      expectedFuncKeys);
}, "casing-values exports");

promise_test(async () => {
  const input = {};
  camelNames.forEach((name, i) => { input[name] = i + 1; });
  const expected = record(input);
  assert_value_equals(values.roundtripRecord(input), expected);
  assert_array_equals(Reflect.ownKeys(values.roundtripRecord(input)), camelNames, "field order");
}, "record fields are named with CamelCase");

promise_test(async () => {
  const kebabInput = {};
  for (const [label] of casingTable) kebabInput[label] = 1;
  const expected = {};
  for (const [label, camel] of casingTable) expected[camel] = label === camel ? 1 : 0;
  assert_value_equals(values.roundtripRecord(kebabInput), record(expected));
}, "record fields under kebab-case keys are missing, and convert from undefined");

promise_test(async () => {
  const input = {};
  camelNames.forEach((name, i) => { input[name] = i % 2 === 0; });
  const expected = flags(input);
  assert_value_equals(values.roundtripFlags(input), expected);
  assert_array_equals(Reflect.ownKeys(values.roundtripFlags(input)), camelNames, "label order");
}, "flags labels are named with CamelCase");

promise_test(async () => {
  for (const label of labels.slice(0, -1)) {
    assert_value_equals(values.roundtripVariant({ kind: label }), variant(label), label);
  }
  assert_value_equals(values.roundtripVariant({ kind: "HTTP-request", value: 5 }),
                      variant("HTTP-request", 5));
}, "variant case kinds are the labels verbatim");

promise_test(async () => {
  for (const [label, camel, pascal] of casingTable) {
    for (const wrong of new Set([camel, pascal])) {
      if (labels.includes(wrong)) continue;
      assert_throws_js(TypeError, () => values.roundtripVariant({ kind: wrong }), wrong);
    }
  }
}, "variant kinds are case-sensitive and not converted");

promise_test(async () => {
  for (const [label] of casingTable) {
    assert_equals(values.roundtripEnum(label), label, label);
  }
}, "enum labels are used verbatim");

promise_test(async () => {
  for (const [label, camel, pascal] of casingTable) {
    for (const wrong of new Set([camel, pascal, label.toLowerCase(), label.toUpperCase()])) {
      if (wrong === label) continue;
      assert_throws_js(TypeError, () => values.roundtripEnum(wrong), wrong);
    }
  }
}, "enum labels are case-sensitive and not converted");
