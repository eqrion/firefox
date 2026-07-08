// |jit-test| include:asserts.js

function conv(kind, jsValue, opts) {
  return new WebIDLValue(new WebIDLType(kind, undefined, opts), jsValue).toValue();
}

assertEq(conv("octet", 256), 0);
assertEq(conv("octet", 257), 1);
assertEq(conv("octet", -1), 255);
assertEq(conv("byte", 128), -128);
assertEq(conv("short", 65536 + 3), 3);
assertEq(conv("long", 4294967296 + 7), 7);

assertEq(conv("long", NaN), 0);
assertEq(conv("long", Infinity), 0);

assertEq(conv("octet", 300, {clamp: true}), 255);
assertEq(conv("octet", -5, {clamp: true}), 0);
assertEq(conv("octet", NaN, {clamp: true}), 0);
// Round half to even.
assertEq(conv("octet", 2.5, {clamp: true}), 2);
assertEq(conv("octet", 3.5, {clamp: true}), 4);

assertEq(conv("octet", 255, {enforceRange: true}), 255);
assertThrowsInstanceOf(() => conv("octet", 256, {enforceRange: true}), TypeError);
assertThrowsInstanceOf(() => conv("byte", -129, {enforceRange: true}), TypeError);
assertThrowsInstanceOf(() => conv("long", Infinity, {enforceRange: true}), TypeError);
