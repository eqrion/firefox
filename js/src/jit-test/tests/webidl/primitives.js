// Round-trip primitive WebIDL types through JS -> IDL -> JS.

function roundtrip(kind, jsValue, opts) {
  let t = new WebIDLType(kind, undefined, opts);
  let v = new WebIDLValue(t, jsValue);
  return v.toValue();
}

assertEq(roundtrip("boolean", 1), true);
assertEq(roundtrip("boolean", 0), false);
assertEq(roundtrip("boolean", ""), false);

assertEq(roundtrip("long", 42), 42);
assertEq(roundtrip("long", -1), -1);
assertEq(roundtrip("unsigned long", -1), 4294967295);

assertEq(roundtrip("double", 3.5), 3.5);
assertEq(roundtrip("double", "2.25"), 2.25);

assertEq(Number.isNaN(roundtrip("unrestricted double", NaN)), true);
assertEq(roundtrip("unrestricted double", Infinity), Infinity);

assertEq(roundtrip("float", 0.1) === Math.fround(0.1), true);

assertEq(roundtrip("DOMString", 123), "123");
assertEq(roundtrip("bigint", 10n), 10n);
