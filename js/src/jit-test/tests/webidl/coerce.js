// |jit-test| include:asserts.js

function make(kind, jsValue, opts) {
  return new WebIDLValue(new WebIDLType(kind, undefined, opts), jsValue);
}

let asDouble = make("long", 5).coerce(new WebIDLType("double"));
assertEq(asDouble.toValue(), 5);

let asOctet = make("long", 300).coerce(new WebIDLType("octet"));
assertEq(asOctet.toValue(), 300 % 256);

assertThrowsInstanceOf(
    () => make("long", 300).coerce(new WebIDLType("octet", undefined,
                                                  {enforceRange: true})),
    TypeError);

assertThrowsInstanceOf(
    () => make("boolean", true).coerce(new WebIDLType("long")), TypeError);

let usv = make("DOMString", String.fromCharCode(0xD800))
              .coerce(new WebIDLType("USVString"));
assertEq(usv.toValue(), "�");

// `undefined`-kind source coerces per WebIDL's "missing/undefined argument"
// conversion rules.
let und = make("undefined", undefined);

assertEq(und.coerce(new WebIDLType("boolean")).toValue(), false);

assertEq(und.coerce(new WebIDLType("long")).toValue(), 0);
assertEq(und.coerce(new WebIDLType("octet", undefined, {clamp: true})).toValue(), 0);
assertThrowsInstanceOf(
    () => und.coerce(new WebIDLType("long", undefined, {enforceRange: true})),
    TypeError);

assertThrowsInstanceOf(() => und.coerce(new WebIDLType("double")), TypeError);
assertEq(Number.isNaN(und.coerce(new WebIDLType("unrestricted double")).toValue()), true);

assertEq(und.coerce(new WebIDLType("DOMString")).toValue(), "undefined");

// Nullable treats an `undefined` source as `null`, not as its inner type's
// own undefined-conversion.
assertEq(und.coerce(new WebIDLType("nullable", new WebIDLType("long"))).toValue(), null);
