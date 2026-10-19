// META: global=window,dedicatedworker,jsshell
// META: script=/wasm/component/jsapi/harness/component-helpers.js
// META: script=/wasm/component/jsapi/harness/assertions.js

// A record, variant, enum or flags type used by an imported or exported
// function must itself be imported or exported as a type. The JS-API has no
// definition for such a type: the import is skipped without reading the
// imports object and the export is left off the exports object.

for_each_entry_point(async (t, instantiate) => {
  const log = [];
  const instance = await instantiate("instance/nonresource-type-import", loggingProxy({}, log));
  assert_array_equals(log, [], "no property is read for a non-resource type import");
  assert_equals(instance.exports.answer(), 42);
}, "a non-resource type import is skipped");

for_each_entry_point(async (t, instantiate) => {
  const instance = await instantiate("instance/nonresource-type-import", { rec: "anything" });
  assert_equals(instance.exports.answer(), 42);
}, "a value supplied for a non-resource type import is ignored");

for_each_entry_point(async (t, instantiate) => {
  const instance = await instantiate("instance/nonresource-type-export", {});
  assert_ComponentInstance(instance, ["answer"]);
  assert_equals(instance.exports.answer(), 42);
}, "a non-resource type export does not appear on the exports object");

promise_test(async () => {
  const instance = await instantiateComponent("exports/object-non-resource-type");
  assert_ComponentInstance(instance, []);
}, "a component exporting only a non-resource type has an empty exports object");

promise_test(async () => {
  const log = [];
  let received;
  const imports = loggingProxy({ f(r) { received = r; return r.x * 2; } }, log);
  const { run } = await componentExports("imports/named-types", imports);
  assert_array_equals(log, ["get f"], "only the function import is read");
  assert_equals(run(), 10);
  assert_value_equals(received, record({ x: 5 }));
}, "a function using an imported named record type still converts the record");
