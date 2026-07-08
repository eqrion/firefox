// |jit-test| skip-if: !wasmComponentsEnabled()

// getWebIDLOperationTypes()/getWebIDLConstructorTypes() expose the recovered
// WebIDLOperationType(s) for a JS function as shell objects for inspection.

// Arity-distinguished overload: two entries, since a method can be
// overloaded.
{
  const ops = getWebIDLOperationTypes(webidlTestOverloaded);
  assertEq(ops.length, 2);

  assertEq(ops[0].paramCount, 1);
  assertEq(ops[0].paramType(0).kind, "long");
  assertEq(ops[0].paramOptional(0), false);
  assertEq(ops[0].paramVariadic(0), false);
  assertEq(ops[0].paramDefault(0), undefined);
  assertEq(ops[0].hasReturnType, true);
  assertEq(ops[0].returnType().kind, "long");

  assertEq(ops[1].paramCount, 2);
  assertEq(ops[1].paramType(1).kind, "long");
}

// Optional param with a declared default: distinct from a required param
// and from an optional param with no default.
{
  const ops = getWebIDLOperationTypes(webidlTestOptionalDefault);
  assertEq(ops.length, 1);
  const op = ops[0];
  assertEq(op.paramCount, 2);

  assertEq(op.paramOptional(0), false);
  assertEq(op.paramDefault(0), undefined);

  assertEq(op.paramOptional(1), true);
  assertEq(op.paramVariadic(1), false);
  const def = op.paramDefault(1);
  assertEq(def instanceof WebIDLValue, true);
}

// Variadic tail: only the last param may set it.
{
  const ops = getWebIDLOperationTypes(webidlTestVariadic);
  assertEq(ops.length, 1);
  const op = ops[0];
  assertEq(op.paramCount, 2);
  assertEq(op.paramVariadic(0), false);
  assertEq(op.paramVariadic(1), true);
  assertEq(op.paramType(1).kind, "long");
}

// Getter/setter: exactly one entry each (attributes are not overloaded).
{
  const getterOps = getWebIDLOperationTypes(webidlTestGetter);
  assertEq(getterOps.length, 1);
  assertEq(getterOps[0].paramCount, 0);
  assertEq(getterOps[0].hasReturnType, true);
  assertEq(getterOps[0].returnType().kind, "DOMString");

  const setterOps = getWebIDLOperationTypes(webidlTestSetter);
  assertEq(setterOps.length, 1);
  assertEq(setterOps[0].paramCount, 1);
  assertEq(setterOps[0].paramType(0).kind, "DOMString");
  assertEq(setterOps[0].hasReturnType, false);
  assertEq(setterOps[0].returnType(), undefined);
}

// Constructor: recovered via GetWebIDLConstructorTypes, not
// GetWebIDLOperationTypes - its return type is the fixture's own interface.
{
  const ctorOps = getWebIDLConstructorTypes(webidlTestConstructor);
  assertEq(ctorOps.length, 1);
  assertEq(ctorOps[0].paramCount, 1);
  assertEq(ctorOps[0].paramType(0).kind, "DOMString");
  assertEq(ctorOps[0].hasReturnType, true);
  assertEq(ctorOps[0].returnType().kind, "interface");
}

// A WebIDL-typed method is not a valid constructor target, and vice versa:
// both fall back rather than misinterpreting the jitinfo.
{
  const ops = getWebIDLConstructorTypes(webidlTestOverloaded);
  assertEq(ops.length, 1);
  assertEq(ops[0].paramVariadic(0), true);

  const ctorAsMethod = getWebIDLOperationTypes(webidlTestConstructor);
  assertEq(ctorAsMethod.length, 1);
  assertEq(ctorAsMethod[0].paramVariadic(0), true);
}

// Fallback: a plain JS function (no jitinfo at all) is not a WebIDL
// operation - GetWebIDLOperationTypes returns the `(any...) -> any`
// pass-through, matching how ComponentCanonLower treats an untyped callee.
{
  function plain(a, b) {}
  const ops = getWebIDLOperationTypes(plain);
  assertEq(ops.length, 1);
  assertEq(ops[0].paramCount, 1);
  assertEq(ops[0].paramType(0).kind, "any");
  assertEq(ops[0].paramVariadic(0), true);
  assertEq(ops[0].hasReturnType, true);
  assertEq(ops[0].returnType().kind, "any");
}

// Same fallback for a non-function passed as a constructor.
{
  const ops = getWebIDLConstructorTypes({});
  assertEq(ops.length, 1);
  assertEq(ops[0].paramVariadic(0), true);
}
