// |jit-test| include:asserts.js

let enumT = new WebIDLType("enumeration", ["red", "green", "blue"]);
assertEq(new WebIDLValue(enumT, "green").toValue(), "green");
assertEq(new WebIDLValue(enumT, "blue").toValue(), "blue");
assertThrowsInstanceOf(() => new WebIDLValue(enumT, "purple"), TypeError);

let nT = new WebIDLType("nullable", new WebIDLType("long"));
assertEq(new WebIDLValue(nT, null).toValue(), null);
assertEq(new WebIDLValue(nT, undefined).toValue(), null);
assertEq(new WebIDLValue(nT, 5).toValue(), 5);
assertEq(new WebIDLValue(nT, 4294967296 + 2).toValue(), 2);
