/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/TestFunctions.h"

#include <cstring>

#include "js/CallAndConstruct.h"     // JS::IsCallable
#include "js/PropertyAndElement.h"   // JS_GetProperty
#include "js/experimental/WebIDL.h"  // JS::GetWebIDLOperationTypes
#include "mozITestInterfaceJS.h"
#include "mozilla/StringBuffer.h"
#include "mozilla/dom/BindingUtils.h"
#include "mozilla/dom/Document.h"
#include "mozilla/dom/TestFunctionsBinding.h"
#include "mozilla/dom/WindowBinding.h"
#include "mozilla/dom/WrapperCachedNonISupportsTestInterface.h"
#include "nsComponentManagerUtils.h"
#include "nsGlobalWindowInner.h"
#include "nsPrintfCString.h"

namespace mozilla::dom {

/* static */
UniquePtr<TestFunctions> TestFunctions::Constructor(GlobalObject& aGlobal) {
  return MakeUnique<TestFunctions>();
}

/* static */
void TestFunctions::ThrowUncatchableException(GlobalObject& aGlobal,
                                              ErrorResult& aRv) {
  aRv.ThrowUncatchableException();
}

/* static */
Promise* TestFunctions::PassThroughPromise(GlobalObject& aGlobal,
                                           Promise& aPromise) {
  return &aPromise;
}

/* static */
already_AddRefed<Promise> TestFunctions::PassThroughCallbackPromise(
    GlobalObject& aGlobal, PromiseReturner& aCallback, ErrorResult& aRv) {
  return aCallback.Call(aRv);
}

void TestFunctions::SetStringData(const nsAString& aString) {
  mStringData = aString;
}

void TestFunctions::GetStringDataAsAString(nsAString& aString) {
  aString = mStringData;
}

void TestFunctions::GetStringDataAsAString(uint32_t aLength,
                                           nsAString& aString) {
  MOZ_RELEASE_ASSERT(aLength <= mStringData.Length(),
                     "Bogus test passing in a too-big length");
  aString.Assign(mStringData.BeginReading(), aLength);
}

void TestFunctions::GetStringDataAsDOMString(const Optional<uint32_t>& aLength,
                                             DOMString& aString) {
  uint32_t length;
  if (aLength.WasPassed()) {
    length = aLength.Value();
    MOZ_RELEASE_ASSERT(length <= mStringData.Length(),
                       "Bogus test passing in a too-big length");
  } else {
    length = mStringData.Length();
  }

  if (StringBuffer* buf = mStringData.GetStringBuffer()) {
    aString.SetKnownLiveStringBuffer(buf, length);
    return;
  }

  // We better have an empty mStringData; otherwise why did we not have a string
  // buffer?
  MOZ_RELEASE_ASSERT(length == 0, "Why no stringbuffer?");
  // No need to do anything here; aString is already empty.
}

void TestFunctions::GetShortLiteralString(nsAString& aString) {
  // JS inline strings can hold 2 * sizeof(void*) chars, which on 32-bit means 8
  // chars.  Return fewer than that.
  aString.AssignLiteral(u"012345");
}

void TestFunctions::GetMediumLiteralString(nsAString& aString) {
  // JS inline strings are at most 2 * sizeof(void*) chars, so at most 16 on
  // 64-bit. nsTAutoString can hold 63 chars in its inline buffer (plus the null
  // terminator). Let's return 40 chars; that way if we ever move to 128-bit
  // void* or something this test will still be valid.
  aString.AssignLiteral(u"0123456789012345678901234567890123456789");
}

void TestFunctions::GetLongLiteralString(nsAString& aString) {
  // Need more than 64 chars.
  aString.AssignLiteral(
      u"0123456789012345678901234567890123456789"  // 40
      "0123456789012345678901234567890123456789"   // 80
  );
}

void TestFunctions::GetStringbufferString(const nsAString& aInput,
                                          nsAString& aRetval) {
  // We have to be a bit careful: if aRetval is an autostring, if we just assign
  // it won't cause stringbuffer allocation.  So we have to round-trip through
  // something that definitely causes a stringbuffer allocation.
  nsString str;
  // Can't use operator= here, because if aInput is a literal string then str
  // would end up the same way.
  str.Assign(aInput.BeginReading(), aInput.Length());

  // Now we might end up hitting our external string cache and getting the wrong
  // sort of external string, so replace the last char by a different value
  // (replacing, not just appending, to preserve the length).  If we have an
  // empty string, our caller screwed up and there's not much we can do for
  // them.
  if (str.Length() > 1) {
    char16_t last = str[str.Length() - 1];
    str.Truncate(str.Length() - 1);
    if (last == 'x') {
      str.Append('y');
    } else {
      str.Append('x');
    }
  }

  // Here we use operator= to preserve stringbufferness.
  aRetval = str;
}

StringType TestFunctions::GetStringType(const nsAString& aString) {
  if (aString.IsLiteral()) {
    return StringType::Literal;
  }

  if (aString.GetStringBuffer()) {
    return StringType::Stringbuffer;
  }

  if (aString.GetDataFlags() & nsAString::DataFlags::INLINE) {
    return StringType::Inline;
  }

  return StringType::Other;
}

bool TestFunctions::StringbufferMatchesStored(const nsAString& aString) {
  return aString.GetStringBuffer() &&
         aString.GetStringBuffer() == mStringData.GetStringBuffer();
}

void TestFunctions::TestThrowNsresult(ErrorResult& aError) {
  nsCOMPtr<mozITestInterfaceJS> impl =
      do_CreateInstance("@mozilla.org/dom/test-interface-js;1");
  aError = impl->TestThrowNsresult();
}

void TestFunctions::TestThrowNsresultFromNative(ErrorResult& aError) {
  nsCOMPtr<mozITestInterfaceJS> impl =
      do_CreateInstance("@mozilla.org/dom/test-interface-js;1");
  aError = impl->TestThrowNsresultFromNative();
}

already_AddRefed<Promise> TestFunctions::ThrowToRejectPromise(
    GlobalObject& aGlobal, ErrorResult& aError) {
  aError.Throw(NS_ERROR_DOM_INVALID_STATE_ERR);
  return nullptr;
}

int32_t TestFunctions::One() const { return 1; }

int32_t TestFunctions::Two() const { return 2; }

void TestFunctions::SetClampedNullableOctet(const Nullable<uint8_t>& aOctet) {
  mClampedNullableOctet = aOctet;
}

Nullable<uint8_t> TestFunctions::GetClampedNullableOctet() const {
  return mClampedNullableOctet;
}

void TestFunctions::SetEnforcedNullableOctet(const Nullable<uint8_t>& aOctet) {
  mEnforcedNullableOctet = aOctet;
}

Nullable<uint8_t> TestFunctions::GetEnforcedNullableOctet() const {
  return mEnforcedNullableOctet;
}

void TestFunctions::SetArrayBufferView(const ArrayBufferView& aBuffer) {}

void TestFunctions::GetArrayBufferView(JSContext* aCx,
                                       JS::Handle<JSObject*> aObj,
                                       JS::MutableHandle<JSObject*> aRetval,
                                       ErrorResult& aError) {
  aError.Throw(NS_ERROR_NOT_IMPLEMENTED);
}

void TestFunctions::SetAllowSharedArrayBufferView(
    const ArrayBufferView& aBuffer) {}

void TestFunctions::GetAllowSharedArrayBufferView(
    JSContext* aCx, JS::Handle<JSObject*> aObj,
    JS::MutableHandle<JSObject*> aRetval, ErrorResult& aError) {
  aError.Throw(NS_ERROR_NOT_IMPLEMENTED);
}

void TestFunctions::SetSequenceOfArrayBufferView(
    const Sequence<ArrayBufferView>& aBuffers) {}

void TestFunctions::GetSequenceOfArrayBufferView(JSContext* aCx,
                                                 JS::Handle<JSObject*> aObj,
                                                 nsTArray<JSObject*>& aRetval,
                                                 ErrorResult& aError) {
  aError.Throw(NS_ERROR_NOT_IMPLEMENTED);
}

void TestFunctions::SetSequenceOfAllowSharedArrayBufferView(
    const Sequence<ArrayBufferView>& aBuffers) {}

void TestFunctions::GetSequenceOfAllowSharedArrayBufferView(
    JSContext* aCx, JS::Handle<JSObject*> aObj, nsTArray<JSObject*>& aRetval,
    ErrorResult& aError) {
  aError.Throw(NS_ERROR_NOT_IMPLEMENTED);
}

void TestFunctions::SetArrayBuffer(const ArrayBuffer& aBuffer) {}

void TestFunctions::GetArrayBuffer(JSContext* aCx, JS::Handle<JSObject*> aObj,
                                   JS::MutableHandle<JSObject*> aRetval,
                                   ErrorResult& aError) {
  aError.Throw(NS_ERROR_NOT_IMPLEMENTED);
}

void TestFunctions::SetAllowSharedArrayBuffer(const ArrayBuffer& aBuffer) {}

void TestFunctions::GetAllowSharedArrayBuffer(
    JSContext* aCx, JS::Handle<JSObject*> aObj,
    JS::MutableHandle<JSObject*> aRetval, ErrorResult& aError) {
  aError.Throw(NS_ERROR_NOT_IMPLEMENTED);
}

void TestFunctions::SetSequenceOfArrayBuffer(
    const Sequence<ArrayBuffer>& aBuffers) {}

void TestFunctions::GetSequenceOfArrayBuffer(JSContext* aCx,
                                             JS::Handle<JSObject*> aObj,
                                             nsTArray<JSObject*>& aRetval,
                                             ErrorResult& aError) {
  aError.Throw(NS_ERROR_NOT_IMPLEMENTED);
}

void TestFunctions::SetSequenceOfAllowSharedArrayBuffer(
    const Sequence<ArrayBuffer>& aBuffers) {}

void TestFunctions::GetSequenceOfAllowSharedArrayBuffer(
    JSContext* aCx, JS::Handle<JSObject*> aObj, nsTArray<JSObject*>& aRetval,
    ErrorResult& aError) {
  aError.Throw(NS_ERROR_NOT_IMPLEMENTED);
}

void TestFunctions::TestNotAllowShared(const ArrayBufferView& aBuffer) {}

void TestFunctions::TestNotAllowShared(const ArrayBuffer& aBuffer) {}

void TestFunctions::TestNotAllowShared(const nsAString& aBuffer) {}

void TestFunctions::TestAllowShared(const ArrayBufferView& aBuffer) {}

void TestFunctions::TestAllowShared(const ArrayBuffer& aBuffer) {}

void TestFunctions::TestDictWithAllowShared(
    const DictWithAllowSharedBufferSource& aDict) {}

void TestFunctions::TestUnionOfBufferSource(
    const ArrayBufferOrArrayBufferViewOrString& aUnion) {}

void TestFunctions::TestUnionOfAllowSharedBufferSource(
    const AllowSharedBufferSource& aUnion) {}

void TestFunctions::TestUnionWithAllowShared(
    const MaybeSharedInt8ArrayOrMaybeSharedInt16Array& aUnion) {}

bool TestFunctions::ObjectFromAboutBlank(JSContext* aCx, JSObject* aObj) {
  // We purposefully don't use WindowOrNull here, because we want to
  // demonstrate the incorrect behavior we get, not just fail some asserts.
  RefPtr<nsGlobalWindowInner> win;
  UNWRAP_MAYBE_CROSS_ORIGIN_OBJECT(Window, aObj, win, aCx);
  if (!win) {
    return false;
  }

  Document* doc = win->GetDoc();
  if (!doc) {
    return false;
  }

  return doc->GetDocumentURI()->GetSpecOrDefault().EqualsLiteral("about:blank");
}

int32_t TestFunctions::ComponentAdd(int32_t aA, int32_t aB) { return aA + aB; }

int32_t TestFunctions::ComponentAdd(int32_t aA, int32_t aB, int32_t aC) {
  return aA + aB + aC;
}

int32_t TestFunctions::ComponentAddDefaulted(int32_t aA, int32_t aB) {
  return aA + aB;
}

bool TestFunctions::ComponentAcceptsSelf(TestFunctions* aOther) {
  return !!aOther;
}

/* static */
int32_t TestFunctions::ComponentStaticAdd(GlobalObject& aGlobal, int32_t aA,
                                          int32_t aB) {
  return aA + aB;
}

/* static */
int32_t TestFunctions::ComponentStaticAdd(GlobalObject& aGlobal, int32_t aA,
                                          int32_t aB, int32_t aC) {
  return aA + aB + aC;
}

void TestFunctions::ComponentEchoUtf8(const nsAString& aS, nsAString& aRetval) {
  aRetval = aS;
}

void TestFunctions::ComponentEchoUtf8(const nsACString& aS,
                                      nsACString& aRetval) {
  aRetval = aS;
}

// Verifies that JS::GetWebIDLOperationTypes recovers the expected shapes
// for the componentAdd/componentAddDefaulted/componentAcceptsSelf/
// componentStaticAdd fixtures above: overload resolution, an optional
// param's default, and a Gecko interface's identity.
bool TestFunctions::CheckWebIDLComponentCall(JSContext* aCx,
                                             JS::Handle<JSObject*> aSelfObj,
                                             ErrorResult& aRv) {
  auto getMethod = [&](const char* aName,
                       JS::MutableHandle<JSObject*> aOutFn) -> bool {
    JS::Rooted<JS::Value> val(aCx);
    if (!JS_GetProperty(aCx, aSelfObj, aName, &val) || !val.isObject() ||
        !JS::IsCallable(&val.toObject())) {
      aRv.ThrowTypeError("CheckWebIDLComponentCall: missing method");
      return false;
    }
    aOutFn.set(&val.toObject());
    return true;
  };

  // A non-static member's operation type carries a leading receiver param
  // typed as the owning interface (see Codegen.py's buildSelfParamStmts), so
  // its declared WebIDL arguments start at kSelfParam. Static members have no
  // such param and start at 0.
  constexpr size_t kSelfParam = 1;
  auto checkSelfParam = [&](const JS::WebIDLOperationType& aOp,
                            const char* aWhat) -> bool {
    if (aOp.params.length() < kSelfParam) {
      aRv.ThrowTypeError(nsPrintfCString("%s: missing self param", aWhat));
      return false;
    }
    const JS::WebIDLType& type = aOp.params[0].type;
    if (type.kind() != JS::WebIDLTypeKind::Interface || !type.interface() ||
        strcmp(type.interface()->name(), "TestFunctions") != 0 ||
        aOp.params[0].optional || aOp.params[0].variadic) {
      aRv.ThrowTypeError(nsPrintfCString("%s: wrong self param", aWhat));
      return false;
    }
    return true;
  };

  JS::Rooted<JSObject*> componentAdd(aCx);
  if (!getMethod("componentAdd", &componentAdd)) {
    return false;
  }
  JS::WebIDLOperationTypeVector addOverloads;
  if (!JS::GetWebIDLOperationTypes(aCx, componentAdd, addOverloads)) {
    return false;
  }
  if (addOverloads.length() != 2) {
    aRv.ThrowTypeError("componentAdd: expected 2 overloads");
    return false;
  }
  for (size_t i = 0; i < addOverloads.length(); i++) {
    const JS::WebIDLOperationType& op = addOverloads[i];
    if (!checkSelfParam(op, "componentAdd")) {
      return false;
    }
    if (op.params.length() != kSelfParam + i + 2) {
      aRv.ThrowTypeError("componentAdd: wrong param count");
      return false;
    }
    for (size_t j = kSelfParam; j < op.params.length(); j++) {
      const JS::WebIDLParam& param = op.params[j];
      if (param.type.kind() != JS::WebIDLTypeKind::Long || param.optional ||
          param.variadic) {
        aRv.ThrowTypeError("componentAdd: wrong param shape");
        return false;
      }
    }
    if (!op.returnType || op.returnType->kind() != JS::WebIDLTypeKind::Long) {
      aRv.ThrowTypeError("componentAdd: wrong return type");
      return false;
    }
  }

  JS::Rooted<JSObject*> addDefaulted(aCx);
  if (!getMethod("componentAddDefaulted", &addDefaulted)) {
    return false;
  }
  JS::WebIDLOperationTypeVector defaultedOverloads;
  if (!JS::GetWebIDLOperationTypes(aCx, addDefaulted, defaultedOverloads)) {
    return false;
  }
  if (defaultedOverloads.length() != 1 ||
      defaultedOverloads[0].params.length() != kSelfParam + 2) {
    aRv.ThrowTypeError("componentAddDefaulted: wrong shape");
    return false;
  }
  if (!checkSelfParam(defaultedOverloads[0], "componentAddDefaulted")) {
    return false;
  }
  const JS::WebIDLParam& p0 = defaultedOverloads[0].params[kSelfParam];
  const JS::WebIDLParam& p1 = defaultedOverloads[0].params[kSelfParam + 1];
  if (p0.optional || p1.type.kind() != JS::WebIDLTypeKind::Long ||
      !p1.optional || p1.variadic || !p1.defaultValue ||
      p1.defaultValue->asSigned() != 100) {
    aRv.ThrowTypeError("componentAddDefaulted: wrong param shape");
    return false;
  }

  JS::Rooted<JSObject*> acceptsSelf(aCx);
  if (!getMethod("componentAcceptsSelf", &acceptsSelf)) {
    return false;
  }
  JS::WebIDLOperationTypeVector acceptsOverloads;
  if (!JS::GetWebIDLOperationTypes(aCx, acceptsSelf, acceptsOverloads)) {
    return false;
  }
  if (acceptsOverloads.length() != 1 ||
      acceptsOverloads[0].params.length() != kSelfParam + 1) {
    aRv.ThrowTypeError("componentAcceptsSelf: wrong shape");
    return false;
  }
  if (!checkSelfParam(acceptsOverloads[0], "componentAcceptsSelf")) {
    return false;
  }
  const JS::WebIDLType& paramType = acceptsOverloads[0].params[kSelfParam].type;
  if (paramType.kind() != JS::WebIDLTypeKind::Nullable) {
    aRv.ThrowTypeError("componentAcceptsSelf: expected nullable param");
    return false;
  }
  const JS::WebIDLType& innerType = paramType.innerType();
  if (innerType.kind() != JS::WebIDLTypeKind::Interface ||
      !innerType.interface() ||
      strcmp(innerType.interface()->name(), "TestFunctions") != 0) {
    aRv.ThrowTypeError("componentAcceptsSelf: wrong interface identity");
    return false;
  }
  if (!innerType.interface()->isOrDerivedFrom(innerType.interface())) {
    aRv.ThrowTypeError("componentAcceptsSelf: isOrDerivedFrom(self) failed");
    return false;
  }

  // componentStaticAdd is defined on the constructor, not the instance.
  JS::Rooted<JS::Value> ctorVal(aCx);
  if (!JS_GetProperty(aCx, aSelfObj, "constructor", &ctorVal) ||
      !ctorVal.isObject()) {
    aRv.ThrowTypeError("componentStaticAdd: no constructor");
    return false;
  }
  JS::Rooted<JSObject*> ctorObj(aCx, &ctorVal.toObject());
  JS::Rooted<JS::Value> staticAddVal(aCx);
  if (!JS_GetProperty(aCx, ctorObj, "componentStaticAdd", &staticAddVal) ||
      !staticAddVal.isObject() || !JS::IsCallable(&staticAddVal.toObject())) {
    aRv.ThrowTypeError("componentStaticAdd: missing method");
    return false;
  }
  JS::Rooted<JSObject*> staticAdd(aCx, &staticAddVal.toObject());
  JS::WebIDLOperationTypeVector staticAddOverloads;
  if (!JS::GetWebIDLOperationTypes(aCx, staticAdd, staticAddOverloads)) {
    return false;
  }
  if (staticAddOverloads.length() != 2) {
    aRv.ThrowTypeError("componentStaticAdd: expected 2 overloads");
    return false;
  }
  // Static, so no leading receiver param: the declared args start at 0, not
  // kSelfParam.
  for (size_t i = 0; i < staticAddOverloads.length(); i++) {
    const JS::WebIDLOperationType& op = staticAddOverloads[i];
    if (op.params.length() != i + 2) {
      aRv.ThrowTypeError("componentStaticAdd: wrong param count");
      return false;
    }
    for (const JS::WebIDLParam& param : op.params) {
      if (param.type.kind() != JS::WebIDLTypeKind::Long || param.optional ||
          param.variadic) {
        aRv.ThrowTypeError("componentStaticAdd: wrong param shape");
        return false;
      }
    }
    if (!op.returnType || op.returnType->kind() != JS::WebIDLTypeKind::Long) {
      aRv.ThrowTypeError("componentStaticAdd: wrong return type");
      return false;
    }
  }

  // The interface object carries the constructor's own operation type, via
  // DOMInterfaceInfo::mConstructorJitInfo. Without that jitinfo this would be
  // the `(any...) -> any` fallback, so checking for TestFunctions' real
  // `constructor()` shape - no params, returning the interface - is what
  // distinguishes the two.
  JS::WebIDLOperationTypeVector ctorOverloads;
  if (!JS::GetWebIDLConstructorTypes(aCx, ctorObj, ctorOverloads)) {
    return false;
  }
  if (ctorOverloads.length() != 1 || !ctorOverloads[0].params.empty()) {
    aRv.ThrowTypeError("constructor: wrong shape");
    return false;
  }
  if (!ctorOverloads[0].returnType ||
      ctorOverloads[0].returnType->kind() != JS::WebIDLTypeKind::Interface ||
      !ctorOverloads[0].returnType->interface() ||
      strcmp(ctorOverloads[0].returnType->interface()->name(),
             "TestFunctions") != 0) {
    aRv.ThrowTypeError("constructor: wrong return type");
    return false;
  }

  // [WasmUtf8] retypes componentEchoUtf8's native-call-entry C++ signature
  // (DOMString -> nsACString) but must leave the WebIDL-visible operation
  // type schema alone, so component overload matching is unaffected - the
  // param/return type recovered here must still read as plain DOMString,
  // never some UTF8String-specific kind (there isn't one; see
  // JS::WebIDLTypeKind).
  JS::Rooted<JSObject*> echoUtf8(aCx);
  if (!getMethod("componentEchoUtf8", &echoUtf8)) {
    return false;
  }
  JS::WebIDLOperationTypeVector echoOverloads;
  if (!JS::GetWebIDLOperationTypes(aCx, echoUtf8, echoOverloads)) {
    return false;
  }
  if (echoOverloads.length() != 1 ||
      echoOverloads[0].params.length() != kSelfParam + 1) {
    aRv.ThrowTypeError("componentEchoUtf8: wrong shape");
    return false;
  }
  if (!checkSelfParam(echoOverloads[0], "componentEchoUtf8")) {
    return false;
  }
  if (echoOverloads[0].params[kSelfParam].type.kind() !=
      JS::WebIDLTypeKind::DOMString) {
    aRv.ThrowTypeError("componentEchoUtf8: wrong param type");
    return false;
  }
  if (!echoOverloads[0].returnType ||
      echoOverloads[0].returnType->kind() != JS::WebIDLTypeKind::DOMString) {
    aRv.ThrowTypeError("componentEchoUtf8: wrong return type");
    return false;
  }

  return true;
}

WrapperCachedNonISupportsTestInterface*
TestFunctions::WrapperCachedNonISupportsObject() {
  if (!mWrapperCachedNonISupportsTestInterface) {
    mWrapperCachedNonISupportsTestInterface =
        new WrapperCachedNonISupportsTestInterface();
  }
  return mWrapperCachedNonISupportsTestInterface;
}

/* static */
already_AddRefed<TestChromeOnlyInterface>
TestFunctions::CreateTestChromeOnlyInterface(GlobalObject& aGlobal) {
  return MakeRefPtr<TestChromeOnlyInterface>(aGlobal.GetAsSupports()).forget();
}

bool TestFunctions::WrapObject(JSContext* aCx,
                               JS::Handle<JSObject*> aGivenProto,
                               JS::MutableHandle<JSObject*> aWrapper) {
  return TestFunctions_Binding::Wrap(aCx, this, aGivenProto, aWrapper);
}

JSObject* TestChromeOnlyInterface::WrapObject(
    JSContext* aCx, JS::Handle<JSObject*> aGivenProto) {
  return TestChromeOnlyInterface_Binding::Wrap(aCx, this, aGivenProto);
}

NS_IMPL_CYCLE_COLLECTION_WRAPPERCACHE(TestChromeOnlyInterface, mParent)

}  // namespace mozilla::dom
