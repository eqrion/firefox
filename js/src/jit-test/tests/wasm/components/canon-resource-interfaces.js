// |jit-test| skip-if: !wasmComponentsEnabled()

// Resources declaring a real WebIDL interface (rather than the generic
// `object` fallback used when a resource has no WebIDL constructor)
// produce an Interface-kind canonical type.
//
// The isOrDerivedFrom identity check against an operation's declared
// interface parameter type (accept the resource's own interface or any
// ancestor, trap on an unrelated one) is exercised only through an explicit
// WebIDLOperationType override on ComponentCanonLower - testing-only
// surface with no instantiation-reachable equivalent (a plain JS import has
// no WebIDL type of its own), so it has no migration target here. See
// canon-resource-native-repr.js for the interface-driven native/wrapped
// classification, which real instantiation does exercise.

const Base = new WebIDLInterface("Base");
const Derived = new WebIDLInterface("Derived", Base);
const Unrelated = new WebIDLInterface("Unrelated");

assertEq(Base.name, "Base");
assertEq(Derived.isOrDerivedFrom(Base), true);
assertEq(Derived.isOrDerivedFrom(Derived), true);
assertEq(Base.isOrDerivedFrom(Derived), false);
assertEq(Unrelated.isOrDerivedFrom(Base), false);

// A resource declaring an interface produces an Interface-kind canonical
// type; one without still falls back to `object`.
{
  const R = new ComponentType("resource", Derived);
  const ownR = new ComponentType("own", R);
  const borrowR = new ComponentType("borrow", R);
  assertEq(ownR.toWebIDLType().kind, "interface");
  assertEq(borrowR.toWebIDLType().kind, "interface");

  const R2 = new ComponentType("resource");
  const ownR2 = new ComponentType("own", R2);
  assertEq(ownR2.toWebIDLType().kind, "object");
}
