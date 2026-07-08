/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jsfriendapi.h"

#include "jsapi-tests/tests.h"

#ifdef ENABLE_WASM_COMPONENTS

// js::GetTestingFunctions installs the WebIDL/component test classes onto a
// fresh object, not onto the global; confirm that constructor-name lookup
// (used by ComponentType.prototype.toWebIDLType) still finds them there.
BEGIN_TEST(testWebIDLTestingFunctionsContainer_toWebIDLTypeRoundTrip) {
  JS::RootedObject testingFns(cx, js::GetTestingFunctions(cx));
  CHECK(testingFns);

  JS::RootedValue testingFnsVal(cx, JS::ObjectValue(*testingFns));
  CHECK(JS_DefineProperty(cx, global, "testingFns", testingFnsVal, 0));

  EXEC(
      "var ct = new testingFns.ComponentType('s32');"
      "var wt = ct.toWebIDLType();"
      "if (!(wt instanceof testingFns.WebIDLType)) throw 'wrong prototype';"
      "if (typeof wt.kind !== 'string') throw 'bad kind';");
  return true;
}
END_TEST(testWebIDLTestingFunctionsContainer_toWebIDLTypeRoundTrip)

#endif  // ENABLE_WASM_COMPONENTS
