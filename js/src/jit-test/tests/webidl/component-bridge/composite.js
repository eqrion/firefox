// |jit-test| skip-if: !wasmComponentsEnabled()

function roundtrip(ct, jsValue) {
  let wt = ct.toWebIDLType();
  let wv = new WebIDLValue(wt, jsValue);
  let cv = new ComponentValue(ct, wv);
  return cv.toWebIDLValue().toValue();
}

{
  let listT = new ComponentType("list", new ComponentType("u32"));
  assertEq(listT.toWebIDLType().kind, "sequence");
  let out = roundtrip(listT, [1, 2, 3]);
  assertEq(Array.isArray(out), true);
  assertEq(out.length, 3);
  assertEq(out[0], 1);
  assertEq(out[2], 3);
}

// Only same-kind tuple members are exercised here: union member resolution
// for tuple types is a placeholder that tries each member type in order, so
// a heterogeneous tuple is not round-trippable yet.
{
  let tupleT = new ComponentType("tuple",
      [new ComponentType("u32"), new ComponentType("u32")]);
  assertEq(tupleT.toWebIDLType().kind, "sequence");
  let out = roundtrip(tupleT, [5, 9]);
  assertEq(out.length, 2);
  assertEq(out[0], 5);
  assertEq(out[1], 9);
}

{
  let recT = new ComponentType("record", [
    ["x", new ComponentType("u32")],
    ["y", new ComponentType("bool")],
  ]);
  assertEq(recT.toWebIDLType().kind, "dictionary");
  let out = roundtrip(recT, {x: 7, y: true});
  assertEq(out.x, 7);
  assertEq(out.y, true);
}

{
  let flagsT = new ComponentType("flags", ["a", "b", "c"]);
  assertEq(flagsT.toWebIDLType().kind, "dictionary");
  let out = roundtrip(flagsT, {a: true, b: false, c: true});
  assertEq(out.a, true);
  assertEq(out.b, false);
  assertEq(out.c, true);
}

{
  let enumT = new ComponentType("enum", ["red", "green", "blue"]);
  assertEq(enumT.toWebIDLType().kind, "enumeration");
  let out = roundtrip(enumT, "blue");
  assertEq(out, "blue");
}

{
  let optT = new ComponentType("option", new ComponentType("u32"));
  assertEq(optT.toWebIDLType().kind, "nullable");
  assertEq(roundtrip(optT, 5), 5);
  assertEq(roundtrip(optT, null), null);
}
