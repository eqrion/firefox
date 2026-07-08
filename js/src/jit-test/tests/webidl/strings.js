// |jit-test| include:asserts.js

function conv(kind, jsValue, opts) {
  return new WebIDLValue(new WebIDLType(kind, undefined, opts), jsValue).toValue();
}

let loneHigh = String.fromCharCode(0xD800);
assertEq(conv("DOMString", loneHigh), loneHigh);
assertEq(conv("DOMString", "hello"), "hello");

assertEq(conv("USVString", loneHigh), "�");
assertEq(conv("USVString", "a" + String.fromCharCode(0xDC00) + "b"), "a�b");
let pair = String.fromCharCode(0xD83D, 0xDE00);
assertEq(conv("USVString", pair), pair);

assertEq(conv("ByteString", "abc"), "abc");
assertThrowsInstanceOf(() => conv("ByteString", "Ā"), TypeError);

assertEq(conv("DOMString", null, {legacyNullToEmptyString: true}), "");
assertEq(conv("DOMString", null), "null");
