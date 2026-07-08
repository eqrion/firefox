// |jit-test| skip-if: !wasmComponentsEnabled()

function roundtrip(kind, jsValue, expectedWebIDLKind) {
  let ct = new ComponentType(kind);
  let wt = ct.toWebIDLType();
  assertEq(wt.kind, expectedWebIDLKind);
  let wv = new WebIDLValue(wt, jsValue);
  let cv = new ComponentValue(ct, wv);
  return cv.toWebIDLValue().toValue();
}

assertEq(roundtrip("bool", true, "boolean"), true);
assertEq(roundtrip("bool", false, "boolean"), false);

assertEq(roundtrip("s8", -5, "byte"), -5);
assertEq(roundtrip("u8", 200, "octet"), 200);
assertEq(roundtrip("s16", -300, "short"), -300);
assertEq(roundtrip("u16", 5000, "unsigned short"), 5000);
assertEq(roundtrip("s32", -70000, "long"), -70000);
assertEq(roundtrip("u32", 70000, "unsigned long"), 70000);
assertEq(roundtrip("s64", -5, "long long"), -5);
assertEq(roundtrip("u64", 5, "unsigned long long"), 5);

assertEq(roundtrip("f32", 1.5, "unrestricted float"), 1.5);
assertEq(roundtrip("f64", 2.5, "unrestricted double"), 2.5);

assertEq(roundtrip("char", "A", "USVString"), "A");
assertEq(roundtrip("string", "hello", "USVString"), "hello");

assertEq(roundtrip("u8", 300, "octet"), 300 % 256);
