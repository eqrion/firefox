/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <utility>

#include "js/experimental/JitInfo.h"  // JSJitInfo, JSWebIDLMethodJitInfo
#include "js/experimental/WebIDL.h"
#include "vm/JSContext.h"   // JSContext, js::ReportOutOfMemory
#include "vm/JSFunction.h"  // JSFunction

using namespace JS;

// Fallback `(any...) -> any` type for a callable with no WebIDL operation
// type of its own.
static bool AppendDefaultOperationType(JSContext* cx,
                                       WebIDLOperationTypeVector& out) {
  WebIDLOperationType opType;
  WebIDLParam param;
  param.type = WebIDLType::primitive(WebIDLTypeKind::Any);
  param.variadic = true;
  if (!opType.params.append(std::move(param))) {
    js::ReportOutOfMemory(cx);
    return false;
  }
  opType.returnType = mozilla::Some(WebIDLType::primitive(WebIDLTypeKind::Any));
  if (!out.append(std::move(opType))) {
    js::ReportOutOfMemory(cx);
    return false;
  }
  return true;
}

static bool IsWebIDLTypedAs(const JSJitInfo* jitInfo, JSJitInfo::OpType type) {
  return jitInfo && jitInfo->isWebIDLTypedJitInfo() && jitInfo->type() == type;
}

JS_PUBLIC_API bool JS::GetWebIDLOperationTypes(JSContext* cx,
                                               JS::HandleObject callable,
                                               WebIDLOperationTypeVector& out) {
  const JSJitInfo* jitInfo = nullptr;
  if (callable->is<JSFunction>()) {
    JSFunction* fun = &callable->as<JSFunction>();
    jitInfo = fun->hasJitInfo() ? fun->jitInfo() : nullptr;
  }
  if (IsWebIDLTypedAs(jitInfo, JSJitInfo::Method) ||
      IsWebIDLTypedAs(jitInfo, JSJitInfo::Getter) ||
      IsWebIDLTypedAs(jitInfo, JSJitInfo::Setter)) {
    const auto* webidlInfo =
        reinterpret_cast<const JSWebIDLMethodJitInfo*>(jitInfo);
    return webidlInfo->buildOperationTypes(cx, out);
  }
  return AppendDefaultOperationType(cx, out);
}

JS_PUBLIC_API bool JS::GetWebIDLConstructorTypes(
    JSContext* cx, JS::HandleObject interfaceObject,
    WebIDLOperationTypeVector& out) {
  const JSJitInfo* jitInfo = nullptr;
  if (interfaceObject->is<JSFunction>()) {
    JSFunction* fun = &interfaceObject->as<JSFunction>();
    jitInfo = fun->hasJitInfo() ? fun->jitInfo() : nullptr;
  }
  if (IsWebIDLTypedAs(jitInfo, JSJitInfo::StaticMethod)) {
    const auto* webidlInfo =
        reinterpret_cast<const JSWebIDLMethodJitInfo*>(jitInfo);
    return webidlInfo->buildOperationTypes(cx, out);
  }
  return AppendDefaultOperationType(cx, out);
}
