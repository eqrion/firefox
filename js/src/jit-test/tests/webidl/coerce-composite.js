// |jit-test| include:asserts.js

function make(type, jsValue) {
  return new WebIDLValue(type, jsValue);
}

const colorEnum = new WebIDLType("enumeration", ["red", "green", "blue"]);
const shadeEnum = new WebIDLType("enumeration", ["green", "blue", "gray"]);

let fromString = make(new WebIDLType("DOMString"), "green").coerce(colorEnum);
assertEq(fromString.toValue(), "green");

let fromEnum = make(colorEnum, "blue").coerce(shadeEnum);
assertEq(fromEnum.toValue(), "blue");

assertThrowsInstanceOf(
    () => make(new WebIDLType("DOMString"), "purple").coerce(colorEnum),
    TypeError);

assertThrowsInstanceOf(
    () => make(new WebIDLType("long"), 1).coerce(colorEnum), TypeError);
assertThrowsInstanceOf(
    () => make(new WebIDLType("undefined"), undefined).coerce(colorEnum),
    TypeError);

const longSeq = new WebIDLType("sequence", new WebIDLType("long"));
const doubleSeq = new WebIDLType("sequence", new WebIDLType("double"));

let seqCoerced = make(longSeq, [1, 2, 3]).coerce(doubleSeq);
assertEq(JSON.stringify(seqCoerced.toValue()), JSON.stringify([1, 2, 3]));

const octetSeq = new WebIDLType("sequence", new WebIDLType("octet"));
let seqElemCoerced = make(longSeq, [300]).coerce(octetSeq);
assertEq(seqElemCoerced.toValue()[0], 300 % 256);

assertThrowsInstanceOf(
    () => make(new WebIDLType("long"), 1).coerce(doubleSeq), TypeError);

const frozenSeq = new WebIDLType("FrozenArray", new WebIDLType("long"));
let toFrozen = make(longSeq, [4, 5]).coerce(frozenSeq);
assertEq(JSON.stringify(toFrozen.toValue()), JSON.stringify([4, 5]));

const longT = new WebIDLType("long");
const dictD1 = new WebIDLType("dictionary",
    [["x", longT, true], ["y", longT, false]]);
const dictD2 = new WebIDLType("dictionary", [["x", longT, true]]);

let d1 = make(dictD1, { x: 1, y: 2 }).coerce(dictD1);
assertEq(d1.toValue().x, 1);
assertEq(d1.toValue().y, 2);

let d2 = make(dictD1, { x: 1 }).coerce(dictD1);
assertEq(d2.toValue().x, 1);
assertEq("y" in d2.toValue(), false);

let d3 = make(dictD1, { x: 1, y: 2 }).coerce(dictD2);
assertEq(d3.toValue().x, 1);
assertEq("y" in d3.toValue(), false);

const dictRequiresY = new WebIDLType("dictionary",
    [["x", longT, true], ["y", longT, true]]);
assertThrowsInstanceOf(
    () => make(dictD2, { x: 1 }).coerce(dictRequiresY), TypeError);

assertThrowsInstanceOf(() => make(longT, 1).coerce(dictD1), TypeError);
