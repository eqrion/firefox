// |jit-test| skip-if: !wasmComponentsEnabled()

// We use imported functions as our oracle for strongly-uniqueness, as only
// functions allow the full range of plain names. A plain name that annotated
// names refer to is imported as a resource type instead.
function funcImport(name) {
  const [, attrs, resource] = /^((?:\[[a-z]+\])*)([^.]*)/.exec(name);
  if (attrs.includes("[constructor]")) {
    return `(import "${name}" (func (result (own $${resource}))))`;
  }
  const self = attrs.includes("[method]") ? `(param "self" (borrow $${resource}))` : "";
  const value = attrs.includes("[set]") ? `(param "v" u32)` : "";
  const result = attrs.includes("[get]") ? "(result u32)" : "";
  return `(import "${name}" (func ${self} ${value} ${result}))`;
}
function imports(names) {
  const resources = new Set(names.map(n => /^(?:\[[a-z]+\])+([^.]+)/.exec(n)?.[1]));
  return names.map(n => resources.has(n)
    ? `(import "${n}" (type $${n} (sub resource)))`
    : funcImport(n)).join("\n");
}
function assertAllStronglyUnique(names) {
  wasmValidateText(`(component ${imports(names)})`);
}
function assertNotStronglyUnique(okNames, badName) {
  assertAllStronglyUnique(okNames);
  wasmFailValidateText(`(component
    ${imports(okNames)}
    (import "${badName}" (func))
  )`, /not strongly-unique/);
}

const specOkExamples = [
  "foo", "foo-bar",
  "[constructor]foo",
  "[method]foo.bar", "[static]foo.baz",
  "[get]prop", "[set]prop",
  "[method][get]foo.prop", "[method][set]foo.prop",
  "[static][get]foo.prop-2", "[static][set]foo.prop-2",
  // For now this is allowed. In the future these are expected to conflict with
  // [method][get]foo.prop and [method][set]foo.prop.
  "[method]foo.get-prop", "[method]foo.set-prop",
];
assertAllStronglyUnique(specOkExamples);

// Conflicts with "foo"
assertNotStronglyUnique(specOkExamples, "foo");
assertNotStronglyUnique(specOkExamples, "FOO");
assertNotStronglyUnique(specOkExamples, "[method]foo.foo");
assertNotStronglyUnique(specOkExamples, "[get]foo");
assertNotStronglyUnique(specOkExamples, "[method][get]foo.foo");
assertNotStronglyUnique(specOkExamples, "[static][set]foo.FOO");

// Conflicts with "foo-bar"
assertNotStronglyUnique(specOkExamples, "foo-BAR");
assertNotStronglyUnique(specOkExamples, "[static]foo-BAR.FOO-bar");

// Conflicts with "[constructor]foo"
assertNotStronglyUnique(specOkExamples, "[constructor]FOO");

// Conflicts with "[method]foo.bar"
assertNotStronglyUnique(specOkExamples, "[method]foo.BAR");
assertNotStronglyUnique(specOkExamples, "[static]foo.bar");

// Conflicts with "[static]foo.baz"
assertNotStronglyUnique(specOkExamples, "[method]foo.baz");

// Conflicts with "[get]prop"
assertNotStronglyUnique(specOkExamples, "prop");

// Conflicts with "[set]prop"
assertNotStronglyUnique(specOkExamples, "[set]PROP");

// Conflicts with "[method][get]foo.prop"
assertNotStronglyUnique(specOkExamples, "[method]foo.prop");
assertNotStronglyUnique(specOkExamples, "[static]foo.PROP");
assertNotStronglyUnique(specOkExamples, "[method][get]foo.PROP");
assertNotStronglyUnique(specOkExamples, "[static][get]foo.prop");

// Conflicts with "[method][set]foo.prop"
assertNotStronglyUnique(specOkExamples, "[method][set]foo.PROP");
assertNotStronglyUnique(specOkExamples, "[static][set]foo.prop");
