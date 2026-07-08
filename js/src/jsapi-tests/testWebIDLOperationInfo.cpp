/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "js/experimental/WebIDL.h"

#include "jsapi-tests/tests.h"

using JS::WebIDLOperationTypeVector;
using JS::WebIDLTypeKind;

static bool NativeStub(JSContext* cx, unsigned argc, JS::Value* vp) {
  JS::CallArgs args = JS::CallArgsFromVp(argc, vp);
  args.rval().setUndefined();
  return true;
}

// A plain native function with no JSJitInfo at all falls back to
// `(any...) -> any` - it is not a WebIDL operation.
BEGIN_TEST(testWebIDLOperationInfo_untypedFunctionFallsBack) {
  JS::Rooted<JSFunction*> fun(cx,
                              JS_NewFunction(cx, NativeStub, 0, 0, "untyped"));
  CHECK(fun);

  WebIDLOperationTypeVector opTypes;
  CHECK(JS::GetWebIDLOperationTypes(cx, fun, opTypes));
  CHECK(opTypes.length() == 1);
  CHECK(opTypes[0].params.length() == 1);
  CHECK(opTypes[0].params[0].type.kind() == WebIDLTypeKind::Any);
  CHECK(opTypes[0].params[0].variadic);
  CHECK(!opTypes[0].params[0].optional);
  CHECK(opTypes[0].returnType.isSome());
  CHECK(opTypes[0].returnType->kind() == WebIDLTypeKind::Any);
  return true;
}
END_TEST(testWebIDLOperationInfo_untypedFunctionFallsBack)

// Same fallback for a non-function object passed as a constructor.
BEGIN_TEST(testWebIDLOperationInfo_nonFunctionConstructorFallsBack) {
  JS::Rooted<JSObject*> obj(cx, JS_NewPlainObject(cx));
  CHECK(obj);

  WebIDLOperationTypeVector opTypes;
  CHECK(JS::GetWebIDLConstructorTypes(cx, obj, opTypes));
  CHECK(opTypes.length() == 1);
  CHECK(opTypes[0].params.length() == 1);
  CHECK(opTypes[0].params[0].variadic);
  return true;
}
END_TEST(testWebIDLOperationInfo_nonFunctionConstructorFallsBack)
