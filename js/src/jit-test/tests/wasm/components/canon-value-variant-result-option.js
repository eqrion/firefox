// |jit-test| skip-if: !wasmComponentsEnabled()

// component<->WebIDL bridge for `variant`, `result` as a plain value type
// (not a function's own return type - see canon-result-throwing.js for
// that), and `option<option<T>>` (which must despecialize to the variant
// form since a WebIDL `null` can't distinguish `none` from `some(none)`).
// Tested directly via ComponentType/ComponentValue, without the
// canon.lift/canon.lower call machinery.

const s32 = new ComponentType("s32");
const str = new ComponentType("string");

function roundtrip(componentType, jsValue) {
  const webIDLValue = new WebIDLValue(componentType.toWebIDLType(), jsValue);
  const componentValue = new ComponentValue(componentType, webIDLValue);
  return componentValue.toWebIDLValue().toValue();
}

// variant with a payload-bearing case and a no-payload case.
{
  const variantType = new ComponentType("variant", [["a", s32], ["b", null]]);

  const a = roundtrip(variantType, { kind: "a", value: 42 });
  assertEq(a.kind, "a");
  assertEq(a.value, 42);

  const b = roundtrip(variantType, { kind: "b" });
  assertEq(b.kind, "b");
  assertEq("value" in b, false);
}

// variant with three cases, exercising a non-trivial enum label set and a
// payload on a case that isn't the first one.
{
  const variantType = new ComponentType("variant",
    [["x", null], ["y", s32], ["z", null]]);

  const y = roundtrip(variantType, { kind: "y", value: -7 });
  assertEq(y.kind, "y");
  assertEq(y.value, -7);

  const z = roundtrip(variantType, { kind: "z" });
  assertEq(z.kind, "z");
  assertEq("value" in z, false);
}

// result<s32, string> as a plain value type.
{
  const resultType = new ComponentType("result", [s32, str]);

  const ok = roundtrip(resultType, { kind: "ok", value: 7 });
  assertEq(ok.kind, "ok");
  assertEq(ok.value, 7);

  const err = roundtrip(resultType, { kind: "error", value: "boom" });
  assertEq(err.kind, "error");
  assertEq(err.value, "boom");
}

// result<_, string>: an ok side with no payload type.
{
  const resultType = new ComponentType("result", [null, str]);

  const ok = roundtrip(resultType, { kind: "ok" });
  assertEq(ok.kind, "ok");
  assertEq("value" in ok, false);

  const err = roundtrip(resultType, { kind: "error", value: "nope" });
  assertEq(err.kind, "error");
  assertEq(err.value, "nope");
}

// option<option<s32>>: despecializes to a variant-dict {none, some(inner)}.
// The "some" case's payload type is CanonicalWebIDLType(option<s32>), i.e.
// a plain nullable long, so `some(none)` is `{kind: "some", value: null}`
// and `some(some(v))` is `{kind: "some", value: v}`.
{
  const innerOption = new ComponentType("option", s32);
  const nestedOption = new ComponentType("option", innerOption);

  const none = roundtrip(nestedOption, { kind: "none" });
  assertEq(none.kind, "none");
  assertEq("value" in none, false);

  const someNone = roundtrip(nestedOption, { kind: "some", value: null });
  assertEq(someNone.kind, "some");
  assertEq(someNone.value, null);

  const someSome = roundtrip(nestedOption, { kind: "some", value: 9 });
  assertEq(someSome.kind, "some");
  assertEq(someSome.value, 9);
}

// A non-nested option is unaffected: still maps straight to nullable.
{
  const optionType = new ComponentType("option", s32);
  assertEq(roundtrip(optionType, 5), 5);
  assertEq(roundtrip(optionType, null), null);
}
