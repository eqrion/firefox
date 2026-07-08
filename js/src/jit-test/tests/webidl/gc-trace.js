// |jit-test| include:asserts.js

// Regression test: WebIDLValue::trace used JS::TraceRoot (root-tracing)
// instead of edge-tracing, which crashed once a long-lived WebIDLValue
// holding a real GC thing survived an incremental mark phase.

const strType = new WebIDLType("DOMString");
const strVal = new WebIDLValue(strType, "hello");
gc();
assertEq(strVal.toValue(), "hello");

const objType = new WebIDLType("object");
const objVal = new WebIDLValue(objType, { a: 1 });
gc();
assertEq(objVal.toValue().a, 1);

const seqType = new WebIDLType("sequence", strType);
const seqVal = new WebIDLValue(seqType, ["a", "b", "c"]);
gc();
assertEq(JSON.stringify(seqVal.toValue()), JSON.stringify(["a", "b", "c"]));

const dictType = new WebIDLType("dictionary", [["s", strType, true]]);
const dictVal = new WebIDLValue(dictType, { s: "world" });
gc();
assertEq(dictVal.toValue().s, "world");

const coerced = strVal.coerce(new WebIDLType("USVString"));
gc();
assertEq(coerced.toValue(), "hello");
