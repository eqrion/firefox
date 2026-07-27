/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "js/ArrayBuffer.h"             // JS::NewArrayBuffer
#include "js/experimental/TypedData.h"  // JS_NewUint8{,Clamped}Array
#include "js/experimental/WebIDL.h"
#include "jsapi-tests/tests.h"

using JS::WebIDLBufferSourceKind;
using JS::WebIDLType;
using JS::WebIDLTypeKind;
using JS::WebIDLValue;

// The buffer-source conversions live in the ungated pure-WebIDL layer
// (js/src/webidl/WebIDLConvert.cpp), so they're exercised here rather than
// through a component call: `canon lower`'s dynamic path builds a
// variadic-`any` operation type and so never presents a declared buffer-source
// param (see WasmComponentLoweredJS), and its native path bypasses
// JS::WebIDLValue entirely.

static JSObject* NewBytes(JSContext* cx, WebIDLBufferSourceKind kind,
                          std::initializer_list<uint8_t> bytes) {
  JS::Rooted<JSObject*> obj(cx);
  switch (kind) {
    case WebIDLBufferSourceKind::ArrayBuffer:
      obj = JS::NewArrayBuffer(cx, bytes.size());
      break;
    case WebIDLBufferSourceKind::Uint8Array:
      obj = JS_NewUint8Array(cx, bytes.size());
      break;
    case WebIDLBufferSourceKind::Uint8ClampedArray:
      obj = JS_NewUint8ClampedArray(cx, bytes.size());
      break;
    default:
      return nullptr;
  }
  if (!obj) {
    return nullptr;
  }
  JS::AutoCheckCannotGC nogc;
  bool isShared;
  mozilla::Span<uint8_t> data =
      kind == WebIDLBufferSourceKind::ArrayBuffer
          ? JS::ArrayBuffer::fromObject(obj).getData(&isShared, nogc)
          : JS::ArrayBufferView::fromObject(obj).getData(&isShared, nogc);
  size_t i = 0;
  for (uint8_t b : bytes) {
    data[i++] = b;
  }
  return obj;
}

// A buffer source is accepted by identity: the IDL value holds the very object
// that came in, and converting back hands the same object out.
BEGIN_TEST(testWebIDLBufferSource_identity) {
  WebIDLType clampedType;
  CHECK(WebIDLType::bufferSource(WebIDLBufferSourceKind::Uint8ClampedArray,
                                 JS::WebIDLTypeFlag_None, &clampedType));

  JS::Rooted<JSObject*> arr(
      cx, NewBytes(cx, WebIDLBufferSourceKind::Uint8ClampedArray, {1, 2, 3}));
  CHECK(arr);
  JS::Rooted<JS::Value> arrVal(cx, JS::ObjectValue(*arr));

  JS::RootedWebIDLValue wv(cx);
  CHECK(JS::JSToWebIDLValue(cx, arrVal, clampedType, &wv));
  CHECK(wv.get().type().kind() == WebIDLTypeKind::BufferSource);
  CHECK(wv.get().asObject() == arr);

  JS::Rooted<JS::Value> back(cx);
  CHECK(JS::WebIDLValueToJS(cx, wv, &back));
  CHECK(back.isObject());
  CHECK(&back.toObject() == arr);
  return true;
}
END_TEST(testWebIDLBufferSource_identity)

// Identity means exactly that - a differently-typed buffer source, or a
// non-buffer-source, is rejected rather than converted.
BEGIN_TEST(testWebIDLBufferSource_wrongKindRejected) {
  WebIDLType clampedType;
  CHECK(WebIDLType::bufferSource(WebIDLBufferSourceKind::Uint8ClampedArray,
                                 JS::WebIDLTypeFlag_None, &clampedType));

  // A Uint8Array is not a Uint8ClampedArray.
  JS::Rooted<JSObject*> plain(
      cx, NewBytes(cx, WebIDLBufferSourceKind::Uint8Array, {1}));
  CHECK(plain);
  JS::Rooted<JS::Value> plainVal(cx, JS::ObjectValue(*plain));
  JS::RootedWebIDLValue wv(cx);
  CHECK(!JS::JSToWebIDLValue(cx, plainVal, clampedType, &wv));
  CHECK(JS_IsExceptionPending(cx));
  JS_ClearPendingException(cx);

  // Neither is a plain object, or a primitive.
  JS::Rooted<JSObject*> obj(cx, JS_NewPlainObject(cx));
  CHECK(obj);
  JS::Rooted<JS::Value> objVal(cx, JS::ObjectValue(*obj));
  CHECK(!JS::JSToWebIDLValue(cx, objVal, clampedType, &wv));
  CHECK(JS_IsExceptionPending(cx));
  JS_ClearPendingException(cx);

  JS::Rooted<JS::Value> num(cx, JS::Int32Value(7));
  CHECK(!JS::JSToWebIDLValue(cx, num, clampedType, &wv));
  CHECK(JS_IsExceptionPending(cx));
  JS_ClearPendingException(cx);
  return true;
}
END_TEST(testWebIDLBufferSource_wrongKindRejected)

// A byte-typed buffer source coerces element-wise to sequence<octet>. This is
// the deliberate cross-overload-class extension that lets a component
// `list<u8>` bridge to a Uint8Array-typed operation.
BEGIN_TEST(testWebIDLBufferSource_coerceToSequence) {
  WebIDLType octet = WebIDLType::primitive(WebIDLTypeKind::Octet);
  WebIDLType seqType;
  CHECK(WebIDLType::sequence(octet, &seqType));

  for (WebIDLBufferSourceKind kind :
       {WebIDLBufferSourceKind::ArrayBuffer, WebIDLBufferSourceKind::Uint8Array,
        WebIDLBufferSourceKind::Uint8ClampedArray}) {
    WebIDLType bufType;
    CHECK(WebIDLType::bufferSource(kind, JS::WebIDLTypeFlag_None, &bufType));
    JS::Rooted<JSObject*> arr(cx, NewBytes(cx, kind, {10, 20, 255}));
    CHECK(arr);

    JS::RootedWebIDLValue src(cx, WebIDLValue::makeObject(bufType, arr));
    JS::RootedWebIDLValue coerced(cx);
    CHECK(JS::CoerceWebIDLValue(cx, src, seqType, &coerced));
    CHECK(coerced.get().type().kind() == WebIDLTypeKind::Sequence);
    const JS::WebIDLValueVector& elems =
        coerced.get().data()->asSequence().elements;
    CHECK(elems.length() == 3);
    CHECK(elems[0].asUnsigned() == 10);
    CHECK(elems[1].asUnsigned() == 20);
    CHECK(elems[2].asUnsigned() == 255);
  }

  // Empty is not a special case, but it is the one where a length-driven loop
  // and a memcpy are easiest to get wrong.
  {
    WebIDLType bufType;
    CHECK(WebIDLType::bufferSource(WebIDLBufferSourceKind::Uint8ClampedArray,
                                   JS::WebIDLTypeFlag_None, &bufType));
    JS::Rooted<JSObject*> arr(
        cx, NewBytes(cx, WebIDLBufferSourceKind::Uint8ClampedArray, {}));
    CHECK(arr);
    JS::RootedWebIDLValue src(cx, WebIDLValue::makeObject(bufType, arr));
    JS::RootedWebIDLValue coerced(cx);
    CHECK(JS::CoerceWebIDLValue(cx, src, seqType, &coerced));
    CHECK(coerced.get().data()->asSequence().elements.length() == 0);
  }
  return true;
}
END_TEST(testWebIDLBufferSource_coerceToSequence)

// The reverse direction: sequence<octet> coerces into a fresh byte-typed
// buffer source of the target kind.
BEGIN_TEST(testWebIDLBufferSource_coerceFromSequence) {
  WebIDLType octet = WebIDLType::primitive(WebIDLTypeKind::Octet);
  WebIDLType seqType;
  CHECK(WebIDLType::sequence(octet, &seqType));

  RefPtr<JS::WebIDLValueData> data =
      js_new<JS::WebIDLValueData>(JS::WebIDLValueData::SequenceData{});
  CHECK(data);
  JS::RootedWebIDLValue src(cx, WebIDLValue::makeComposite(seqType, data));
  for (uint8_t b : {uint8_t(3), uint8_t(4), uint8_t(200)}) {
    CHECK(data->appendSequenceElement(WebIDLValue::makeUnsigned(octet, b)));
  }

  WebIDLType clampedType;
  CHECK(WebIDLType::bufferSource(WebIDLBufferSourceKind::Uint8ClampedArray,
                                 JS::WebIDLTypeFlag_None, &clampedType));
  JS::RootedWebIDLValue coerced(cx);
  CHECK(JS::CoerceWebIDLValue(cx, src, clampedType, &coerced));
  CHECK(coerced.get().type().kind() == WebIDLTypeKind::BufferSource);

  JS::Rooted<JSObject*> result(cx, coerced.get().asObject());
  CHECK(result);
  {
    JS::AutoCheckCannotGC nogc;
    bool isShared;
    mozilla::Span<uint8_t> bytes =
        JS::ArrayBufferView::fromObject(result).getData(&isShared, nogc);
    CHECK(bytes.Length() == 3);
    CHECK(bytes[0] == 3);
    CHECK(bytes[1] == 4);
    CHECK(bytes[2] == 200);
  }
  return true;
}
END_TEST(testWebIDLBufferSource_coerceFromSequence)

// Only byte-typed buffer sources bridge. A generic ArrayBufferView has no
// statically known element type, so a memcpy would be unsound and the
// coercion is refused in both directions.
BEGIN_TEST(testWebIDLBufferSource_nonByteKindRejected) {
  WebIDLType octet = WebIDLType::primitive(WebIDLTypeKind::Octet);
  WebIDLType seqType;
  CHECK(WebIDLType::sequence(octet, &seqType));
  WebIDLType viewType;
  CHECK(WebIDLType::bufferSource(WebIDLBufferSourceKind::ArrayBufferView,
                                 JS::WebIDLTypeFlag_None, &viewType));

  JS::Rooted<JSObject*> arr(
      cx, NewBytes(cx, WebIDLBufferSourceKind::Uint8Array, {1, 2}));
  CHECK(arr);

  JS::RootedWebIDLValue asView(cx, WebIDLValue::makeObject(viewType, arr));
  JS::RootedWebIDLValue coerced(cx);
  CHECK(!JS::CoerceWebIDLValue(cx, asView, seqType, &coerced));
  CHECK(JS_IsExceptionPending(cx));
  JS_ClearPendingException(cx);

  RefPtr<JS::WebIDLValueData> data =
      js_new<JS::WebIDLValueData>(JS::WebIDLValueData::SequenceData{});
  CHECK(data);
  JS::RootedWebIDLValue src(cx, WebIDLValue::makeComposite(seqType, data));
  CHECK(data->appendSequenceElement(WebIDLValue::makeUnsigned(octet, 1)));
  CHECK(!JS::CoerceWebIDLValue(cx, src, viewType, &coerced));
  CHECK(JS_IsExceptionPending(cx));
  JS_ClearPendingException(cx);
  return true;
}
END_TEST(testWebIDLBufferSource_nonByteKindRejected)
