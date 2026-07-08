// |jit-test| skip-if: !wasmComponentsEnabled()
// The ComponentValue constructor requires the WebIDLValue passed to it to
// already be of the canonical WebIDL type for the target ComponentType, and
// throws instead of converting a mismatched value.

assertThrowsInstanceOf(
    () => new ComponentValue(new ComponentType("u32"),
                             new WebIDLValue(new WebIDLType("double"), 1)),
    TypeError);

{
  let listT = new ComponentType("list", new ComponentType("u32"));
  assertThrowsInstanceOf(
      () => new ComponentValue(
          listT, new WebIDLValue(new WebIDLType("unsigned long"), 5)),
      TypeError);
}

{
  let listT = new ComponentType("list", new ComponentType("u32"));
  let wrongElemT = new WebIDLType("sequence", new WebIDLType("DOMString"));
  assertThrowsInstanceOf(
      () => new ComponentValue(listT, new WebIDLValue(wrongElemT, ["a"])),
      TypeError);
}
