// |jit-test| include:asserts.js

let longT = new WebIDLType("long");
let boolT = new WebIDLType("boolean");

let seqT = new WebIDLType("sequence", longT);
let seq = new WebIDLValue(seqT, [1, 2, 3]).toValue();
assertEq(Array.isArray(seq), true);
assertEq(seq.length, 3);
assertEq(seq[0], 1);
assertEq(seq[2], 3);

let useqT = new WebIDLType("sequence", new WebIDLType("unsigned long"));
let useq = new WebIDLValue(useqT, [-1, 0]).toValue();
assertEq(useq[0], 4294967295);

let fromSet = new WebIDLValue(seqT, new Set([5, 6])).toValue();
assertEq(fromSet.length, 2);
assertEq(fromSet[0], 5);

let dictT = new WebIDLType("dictionary", [
  ["x", longT, true],
  ["y", boolT, false],
]);

let d1 = new WebIDLValue(dictT, {x: 7, y: 1}).toValue();
assertEq(d1.x, 7);
assertEq(d1.y, true);

let d2 = new WebIDLValue(dictT, {x: 9}).toValue();
assertEq(d2.x, 9);
assertEq("y" in d2, false);

assertThrowsInstanceOf(() => new WebIDLValue(dictT, {y: 1}), TypeError);
