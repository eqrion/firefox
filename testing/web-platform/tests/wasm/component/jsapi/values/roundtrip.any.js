// META: global=window,dedicatedworker,jsshell
// META: script=/wasm/component/jsapi/harness/component-helpers.js
// META: script=/wasm/component/jsapi/harness/assertions.js
// META: script=/wasm/component/jsapi/harness/value-cases.js

// ToJSValue(ToComponentValue(v)) is the identity on already-converted values,
// except for map duplicate keys and NaN canonicalisation. Each table entry is
// driven through a passthrough fixture whose every import is an identity echo.

function describe(value) {
  if (typeof value === "string") return JSON.stringify(value);
  if (typeof value === "bigint") return `${value}n`;
  if (typeof value !== "object" || value === null) return String(value);
  if (value instanceof Map) return `Map(${[...value].map(([k, v]) => `${describe(k)} => ${describe(v)}`).join(", ")})`;
  if (ArrayBuffer.isView(value)) return `${value.constructor.name}[${[...value].join(", ")}]`;
  if (Array.isArray(value)) return `[${value.map(describe).join(", ")}]`;
  return `{${Reflect.ownKeys(value).map(k => `${String(k)}: ${describe(value[k])}`).join(", ")}}`;
}

for (const { fixture, exportName, cases } of valueCases) {
  let roundtrip;
  let received;
  promise_setup(async () => {
    const echo = x => { received = x; return x; };
    const imports = new Proxy({}, { get: () => echo });
    ({ [exportName]: roundtrip } = await componentExports(fixture, imports));
  });

  cases.forEach(([input, ...rest], i) => {
    const expected = rest.length > 0 ? rest[0] : input;
    const name = `${fixture} ${exportName} #${i} ${describe(input)}`;
    promise_test(async () => {
      const result = roundtrip(input);
      assert_value_equals(received, expected, "value seen by echo");
      assert_value_equals(result, expected, "result");
    }, name);

    promise_test(async () => {
      const once = roundtrip(input);
      const twice = roundtrip(once);
      assert_value_equals(twice, once, "second roundtrip");
    }, `${name}: converting the result again is the identity`);
  });
}
