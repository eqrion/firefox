/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/Vector.h"  // mozilla::Vector

#include <cmath>
#include <cstring>

#include "jsapi.h"  // JS_New*, JS_EncodeStringToUTF8, JS_ReportError*

#include "js/Array.h"        // JS::NewArrayObject
#include "js/ArrayBuffer.h"  // JS::NewArrayBuffer, JS::ArrayBuffer
#include "js/BigInt.h"       // JS::ToBigInt
#include "js/Conversions.h"  // JS::ToNumber, JS::ToBoolean, JS::ToString
#include "js/experimental/TypedData.h"  // JS_NewUint8{,Clamped}Array, JS::ArrayBufferOrView
#include "js/experimental/WebIDL.h"
#include "js/ForOfIterator.h"         // JS::ForOfIterator
#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#include "js/PropertyAndElement.h"    // JS_GetProperty, JS_SetProperty
#include "js/SharedArrayBuffer.h"     // JS::IsSharedArrayBufferObject
#include "js/Utility.h"               // js_new
#include "util/StringBuilder.h"       // js::StringBuilder
#include "vm/BigIntType.h"            // JS::BigInt
#include "vm/JSContext.h"             // JSContext, js::ReportOutOfMemory
#include "vm/StringType.h"            // JSString, JSLinearString

using namespace JS;

static bool ThrowConversionError(JSContext* cx, const char* what) {
  JS_ReportErrorNumberASCII(cx, js::GetErrorMessage, nullptr,
                            JSMSG_CANT_CONVERT_TO, what, "WebIDL type");
  return false;
}

// Whether `obj` (possibly a cross-compartment wrapper) is of the buffer-source
// kind `kind` demands. A buffer source is accepted by identity, so this is a
// class test rather than a coercion.
static bool MatchesBufferSourceKind(JSObject* obj,
                                    WebIDLBufferSourceKind kind) {
  switch (kind) {
    case WebIDLBufferSourceKind::ArrayBuffer:
      return !!JS::ArrayBuffer::unwrap(obj);
    case WebIDLBufferSourceKind::ArrayBufferView:
      return !!JS::ArrayBufferView::unwrap(obj);
    case WebIDLBufferSourceKind::Uint8Array:
      return !!JS::Uint8Array::unwrap(obj);
    case WebIDLBufferSourceKind::Uint8ClampedArray:
      return !!JS::Uint8ClampedArray::unwrap(obj);
  }
  MOZ_CRASH("bad WebIDLBufferSourceKind");
}

// Whether `obj`'s backing store is a SharedArrayBuffer, for enforcing the
// absence of `[AllowShared]`. `obj` must already have matched `kind` via
// MatchesBufferSourceKind.
//
// TODO(webidl): a DataView reaching the generic ArrayBufferView kind reports
// false, so a shared one slips through. Recovering its sharedness needs
// JS_GetArrayBufferViewBuffer, which can throw. Not a gap in practice yet: only
// the byte kinds below bridge to a component `list<u8>` (see
// WebIDLBufferSourceKindIsBytes), and the generic view kind traps in
// BuildCoerceStep regardless.
static bool IsSharedBufferSource(JSObject* obj, WebIDLBufferSourceKind kind) {
  JSObject* unwrapped =
      JS::ArrayBufferOrView::unwrap(obj).asObjectUnbarriered();
  MOZ_ASSERT(unwrapped, "caller checked MatchesBufferSourceKind");
  if (kind == WebIDLBufferSourceKind::ArrayBuffer) {
    return JS::IsSharedArrayBufferObject(unwrapped);
  }
  return JS_IsTypedArrayObject(unwrapped) &&
         JS_GetTypedArraySharedness(unwrapped);
}

JS_PUBLIC_API WebIDLOverloadClass
JS::WebIDLOverloadKind(const WebIDLType& type) {
  WebIDLTypeKind kind = type.kind();
  if (WebIDLTypeKindIsNumeric(kind)) {
    return WebIDLOverloadClass::Numeric;
  }
  if (WebIDLTypeKindIsString(kind) || kind == WebIDLTypeKind::Enumeration) {
    return WebIDLOverloadClass::String;
  }
  switch (kind) {
    case WebIDLTypeKind::Any:
      return WebIDLOverloadClass::Any;
    case WebIDLTypeKind::Undefined:
      return WebIDLOverloadClass::Undefined;
    case WebIDLTypeKind::Boolean:
      return WebIDLOverloadClass::Boolean;
    case WebIDLTypeKind::BigInt:
      return WebIDLOverloadClass::BigInt;
    case WebIDLTypeKind::Symbol:
      return WebIDLOverloadClass::Symbol;
    default:
      // Remaining kinds (including Nullable) are treated as the object
      // overload class for distinguishability purposes.
      return WebIDLOverloadClass::Object;
  }
}

static void IntegerWidthAndSign(WebIDLTypeKind kind, int* bits,
                                bool* isSigned) {
  switch (kind) {
    case WebIDLTypeKind::Byte:
      *bits = 8;
      *isSigned = true;
      break;
    case WebIDLTypeKind::Octet:
      *bits = 8;
      *isSigned = false;
      break;
    case WebIDLTypeKind::Short:
      *bits = 16;
      *isSigned = true;
      break;
    case WebIDLTypeKind::UnsignedShort:
      *bits = 16;
      *isSigned = false;
      break;
    case WebIDLTypeKind::Long:
      *bits = 32;
      *isSigned = true;
      break;
    case WebIDLTypeKind::UnsignedLong:
      *bits = 32;
      *isSigned = false;
      break;
    case WebIDLTypeKind::LongLong:
      *bits = 64;
      *isSigned = true;
      break;
    case WebIDLTypeKind::UnsignedLongLong:
      *bits = 64;
      *isSigned = false;
      break;
    default:
      MOZ_CRASH("not an integer kind");
  }
}

static void IntegerBounds(int bits, bool isSigned, double* min, double* max) {
  if (isSigned) {
    *min = -std::ldexp(1.0, bits - 1);
    *max = std::ldexp(1.0, bits - 1) - 1.0;
  } else {
    *min = 0.0;
    *max = std::ldexp(1.0, bits) - 1.0;
  }
  // WebIDL clamps long long / unsigned long long to the 2^53 safe-integer
  // range.
  if (bits == 64) {
    double safe = std::ldexp(1.0, 53) - 1.0;
    if (isSigned) {
      *min = -safe;
      *max = safe;
    } else {
      *max = safe;
    }
  }
}

static bool StoreInteger(JSContext* cx, double d, const WebIDLType& type,
                         MutableHandleWebIDLValue out) {
  int bits;
  bool isSigned;
  IntegerWidthAndSign(type.kind(), &bits, &isSigned);

  double lo, hi;
  IntegerBounds(bits, isSigned, &lo, &hi);

  if (type.hasEnforceRange()) {
    if (!std::isfinite(d)) {
      return ThrowConversionError(cx, "non-finite value for [EnforceRange]");
    }
    double r = std::trunc(d);
    if (r < lo || r > hi) {
      return ThrowConversionError(cx, "out-of-range value for [EnforceRange]");
    }
    if (isSigned) {
      out.set(WebIDLValue::makeSigned(type, int64_t(r)));
    } else {
      out.set(WebIDLValue::makeUnsigned(type, uint64_t(r)));
    }
    return true;
  }

  if (type.hasClamp()) {
    double v;
    if (std::isnan(d)) {
      v = 0.0;
    } else if (d <= lo) {
      v = lo;
    } else if (d >= hi) {
      v = hi;
    } else {
      // Round half to even.
      double toTruncate = d < 0 ? d - 0.5 : d + 0.5;
      double truncated = std::trunc(toTruncate);
      if (truncated == toTruncate && std::fmod(truncated, 2.0) != 0.0) {
        truncated += (truncated < 0 ? 1.0 : -1.0);
      }
      v = truncated;
    }
    if (isSigned) {
      out.set(WebIDLValue::makeSigned(type, int64_t(v)));
    } else {
      out.set(WebIDLValue::makeUnsigned(type, uint64_t(v)));
    }
    return true;
  }

  // Default: modular reduction.
  if (!std::isfinite(d)) {
    d = 0.0;
  }
  double t = std::trunc(d);
  if (bits <= 32) {
    double modulus = std::ldexp(1.0, bits);
    double m = std::fmod(t, modulus);
    if (m < 0) {
      m += modulus;
    }
    if (isSigned && m >= modulus / 2) {
      m -= modulus;
      out.set(WebIDLValue::makeSigned(type, int64_t(m)));
    } else if (isSigned) {
      out.set(WebIDLValue::makeSigned(type, int64_t(m)));
    } else {
      out.set(WebIDLValue::makeUnsigned(type, uint64_t(m)));
    }
    return true;
  }

  // TODO(webidl): exact modular reduction for the full 64-bit range.
  if (isSigned) {
    out.set(WebIDLValue::makeSigned(type, int64_t(t)));
  } else {
    out.set(WebIDLValue::makeUnsigned(type, uint64_t(t)));
  }
  return true;
}

static bool StoreFloat(JSContext* cx, double d, const WebIDLType& type,
                       MutableHandleWebIDLValue out) {
  bool restricted = !WebIDLTypeKindIsUnrestrictedFloat(type.kind());
  if (restricted && !std::isfinite(d)) {
    return ThrowConversionError(cx, "non-finite value for restricted float");
  }
  // Single-precision types round to float first.
  if (type.kind() == WebIDLTypeKind::Float ||
      type.kind() == WebIDLTypeKind::UnrestrictedFloat) {
    d = double(float(d));
  }
  out.set(WebIDLValue::makeDouble(type, d));
  return true;
}

// Produce a USVString: replace lone surrogates in `str` with U+FFFD.
static JSString* NormalizeToUSV(JSContext* cx, JSString* str) {
  JSLinearString* linear = str->ensureLinear(cx);
  if (!linear) {
    return nullptr;
  }
  size_t len = linear->length();

  js::JSStringBuilder sb(cx);
  if (!sb.reserve(len)) {
    return nullptr;
  }
  for (size_t i = 0; i < len; i++) {
    char16_t c = linear->latin1OrTwoByteChar(i);
    if (c >= 0xD800 && c <= 0xDBFF) {
      // High surrogate: valid only if followed by a low surrogate.
      if (i + 1 < len) {
        char16_t next = linear->latin1OrTwoByteChar(i + 1);
        if (next >= 0xDC00 && next <= 0xDFFF) {
          if (!sb.append(c) || !sb.append(next)) {
            return nullptr;
          }
          i++;
          continue;
        }
      }
      if (!sb.append(char16_t(0xFFFD))) {
        return nullptr;
      }
    } else if (c >= 0xDC00 && c <= 0xDFFF) {
      // Lone low surrogate.
      if (!sb.append(char16_t(0xFFFD))) {
        return nullptr;
      }
    } else {
      if (!sb.append(c)) {
        return nullptr;
      }
    }
  }
  return sb.finishString();
}

static mozilla::Maybe<uint32_t> FindDictionaryMemberIndex(
    const WebIDLDictionaryType& dict, const char* name) {
  for (uint32_t i = 0; i < dict.members.length(); i++) {
    if (std::strcmp(dict.members[i].name.get(), name) == 0) {
      return mozilla::Some(i);
    }
  }
  return mozilla::Nothing();
}

// Verify a ByteString: every code unit must be <= 0xFF.
static bool CheckByteString(JSContext* cx, JSString* str) {
  JSLinearString* linear = str->ensureLinear(cx);
  if (!linear) {
    return false;
  }
  if (linear->hasLatin1Chars()) {
    return true;
  }
  size_t len = linear->length();
  for (size_t i = 0; i < len; i++) {
    if (linear->latin1OrTwoByteChar(i) > 0xFF) {
      return ThrowConversionError(cx, "code unit out of range for ByteString");
    }
  }
  return true;
}

static bool JSToWebIDLValueImpl(JSContext* cx, HandleValue value,
                                const WebIDLType& type,
                                MutableHandleWebIDLValue out);

static bool JSToWebIDLString(JSContext* cx, HandleValue value,
                             const WebIDLType& type,
                             MutableHandleWebIDLValue out) {
  RootedString str(cx);
  if (value.isNull() && type.hasLegacyNullToEmptyString()) {
    str = JS_NewStringCopyZ(cx, "");
    if (!str) {
      return false;
    }
  } else {
    str = JS::ToString(cx, value);
    if (!str) {
      return false;
    }
  }
  switch (type.kind()) {
    case WebIDLTypeKind::DOMString:
      out.set(WebIDLValue::makeString(type, str));
      return true;
    case WebIDLTypeKind::USVString: {
      JSString* usv = NormalizeToUSV(cx, str);
      if (!usv) {
        return false;
      }
      out.set(WebIDLValue::makeString(type, usv));
      return true;
    }
    case WebIDLTypeKind::ByteString:
      if (!CheckByteString(cx, str)) {
        return false;
      }
      out.set(WebIDLValue::makeString(type, str));
      return true;
    default:
      MOZ_CRASH("not a string kind");
  }
}

static bool JSToWebIDLSequence(JSContext* cx, HandleValue value,
                               const WebIDLType& type,
                               MutableHandleWebIDLValue out) {
  JS::ForOfIterator iter(cx);
  if (!iter.init(value)) {
    return false;
  }
  WebIDLType element = type.innerType();

  // Allocate the (empty) data and immediately root it via `out`, so that GC
  // edges appended below survive intervening allocations.
  RefPtr<WebIDLValueData> data =
      js_new<WebIDLValueData>(WebIDLValueData::SequenceData{});
  if (!data) {
    js::ReportOutOfMemory(cx);
    return false;
  }
  out.set(WebIDLValue::makeComposite(type, data));

  RootedValue elemVal(cx);
  RootedWebIDLValue elemOut(cx);
  while (true) {
    bool done = false;
    if (!iter.next(&elemVal, &done)) {
      return false;
    }
    if (done) {
      break;
    }
    if (!JSToWebIDLValueImpl(cx, elemVal, element, &elemOut)) {
      return false;
    }
    if (!data->appendSequenceElement(elemOut.get())) {
      js::ReportOutOfMemory(cx);
      return false;
    }
  }
  return true;
}

static bool JSToWebIDLDictionary(JSContext* cx, HandleValue value,
                                 const WebIDLType& type,
                                 MutableHandleWebIDLValue out) {
  if (!value.isObject() && !value.isNullOrUndefined()) {
    return ThrowConversionError(cx, "dictionary");
  }
  RootedObject obj(cx);
  if (value.isObject()) {
    obj = &value.toObject();
  }
  const WebIDLDictionaryType& dict =
      type.def()->schema().as<WebIDLDictionaryType>();

  RefPtr<WebIDLValueData> data =
      js_new<WebIDLValueData>(WebIDLValueData::DictionaryData{});
  if (!data) {
    js::ReportOutOfMemory(cx);
    return false;
  }
  out.set(WebIDLValue::makeComposite(type, data));

  RootedValue memberVal(cx);
  RootedWebIDLValue memberOut(cx);
  for (const WebIDLDictionaryMember& member : dict.members) {
    memberVal.setUndefined();
    if (obj) {
      if (!JS_GetProperty(cx, obj, member.name.get(), &memberVal)) {
        return false;
      }
    }
    if (memberVal.isUndefined()) {
      if (member.required) {
        return ThrowConversionError(cx, "missing required dictionary member");
      }
      // Absent optional member: store a default (Invalid-typed) placeholder,
      // which WebIDLValueToJS skips.
      if (!data->appendDictionaryMember(WebIDLValue())) {
        js::ReportOutOfMemory(cx);
        return false;
      }
      continue;
    }
    if (!JSToWebIDLValueImpl(cx, memberVal, member.type, &memberOut)) {
      return false;
    }
    if (!data->appendDictionaryMember(memberOut.get())) {
      js::ReportOutOfMemory(cx);
      return false;
    }
  }
  return true;
}

static bool JSToWebIDLValueImpl(JSContext* cx, HandleValue value,
                                const WebIDLType& type,
                                MutableHandleWebIDLValue out) {
  switch (type.kind()) {
    case WebIDLTypeKind::Any:
      out.set(WebIDLValue::makeAny(type, value));
      return true;

    case WebIDLTypeKind::Undefined:
      out.set(WebIDLValue::makeUndefined(type));
      return true;

    case WebIDLTypeKind::Boolean:
      out.set(WebIDLValue::makeBool(type, JS::ToBoolean(value)));
      return true;

    case WebIDLTypeKind::Byte:
    case WebIDLTypeKind::Octet:
    case WebIDLTypeKind::Short:
    case WebIDLTypeKind::UnsignedShort:
    case WebIDLTypeKind::Long:
    case WebIDLTypeKind::UnsignedLong:
    case WebIDLTypeKind::LongLong:
    case WebIDLTypeKind::UnsignedLongLong: {
      double d;
      if (!JS::ToNumber(cx, value, &d)) {
        return false;
      }
      return StoreInteger(cx, d, type, out);
    }

    case WebIDLTypeKind::Float:
    case WebIDLTypeKind::UnrestrictedFloat:
    case WebIDLTypeKind::Double:
    case WebIDLTypeKind::UnrestrictedDouble: {
      double d;
      if (!JS::ToNumber(cx, value, &d)) {
        return false;
      }
      return StoreFloat(cx, d, type, out);
    }

    case WebIDLTypeKind::BigInt: {
      RootedBigInt bi(cx, JS::ToBigInt(cx, value));
      if (!bi) {
        return false;
      }
      out.set(WebIDLValue::makeBigInt(type, bi));
      return true;
    }

    case WebIDLTypeKind::DOMString:
    case WebIDLTypeKind::USVString:
    case WebIDLTypeKind::ByteString:
      return JSToWebIDLString(cx, value, type, out);

    case WebIDLTypeKind::Object:
      if (!value.isObject()) {
        return ThrowConversionError(cx, "object");
      }
      out.set(WebIDLValue::makeObject(type, &value.toObject()));
      return true;

    case WebIDLTypeKind::Interface: {
      if (!value.isObject()) {
        return ThrowConversionError(cx, "interface");
      }
      // NOTE(webidl): identity/brand-checking of the object against the
      // interface is not modeled yet; we accept any object.
      out.set(WebIDLValue::makeObject(type, &value.toObject()));
      return true;
    }

    case WebIDLTypeKind::Enumeration: {
      RootedString str(cx, JS::ToString(cx, value));
      if (!str) {
        return false;
      }
      JS::UniqueChars label = JS_EncodeStringToUTF8(cx, str);
      if (!label) {
        return false;
      }
      const WebIDLEnumerationType& en =
          type.def()->schema().as<WebIDLEnumerationType>();
      mozilla::Maybe<uint32_t> index =
          en.find(label.get(), std::strlen(label.get()));
      if (index.isNothing()) {
        return ThrowConversionError(cx, "invalid enumeration value");
      }
      out.set(WebIDLValue::makeEnum(type, *index));
      return true;
    }

    case WebIDLTypeKind::Nullable:
      if (value.isNullOrUndefined()) {
        out.set(WebIDLValue::makeNull(type));
        return true;
      }
      return JSToWebIDLValueImpl(cx, value, type.innerType(), out);

    case WebIDLTypeKind::Sequence:
    case WebIDLTypeKind::FrozenArray:
    case WebIDLTypeKind::ObservableArray:
      return JSToWebIDLSequence(cx, value, type, out);

    case WebIDLTypeKind::Dictionary:
      return JSToWebIDLDictionary(cx, value, type, out);

    case WebIDLTypeKind::Union: {
      const WebIDLUnionType& un = type.def()->schema().as<WebIDLUnionType>();
      // TODO(webidl): proper union resolution per the distinguishable-type
      // algorithm. For now, approximate it by preferring members whose
      // overload class matches the value's kind, since a maximally
      // permissive member (e.g. numeric, which never throws) would otherwise
      // always win and mistype the result.
      auto tryMember = [&](const WebIDLType& member) {
        if (JSToWebIDLValueImpl(cx, value, member, out)) {
          return true;
        }
        if (cx->isExceptionPending()) {
          cx->clearPendingException();
        }
        return false;
      };
      auto matchesValueKind = [&](const WebIDLType& member) {
        WebIDLOverloadClass cls = WebIDLOverloadKind(member);
        if (value.isString()) {
          return cls == WebIDLOverloadClass::String;
        }
        if (value.isNumber()) {
          return cls == WebIDLOverloadClass::Numeric;
        }
        if (value.isBoolean()) {
          return cls == WebIDLOverloadClass::Boolean;
        }
        if (value.isBigInt()) {
          return cls == WebIDLOverloadClass::BigInt;
        }
        if (value.isUndefined()) {
          return cls == WebIDLOverloadClass::Undefined;
        }
        return false;
      };
      for (const WebIDLType& member : un.members) {
        if (matchesValueKind(member) && tryMember(member)) {
          return true;
        }
      }
      for (const WebIDLType& member : un.members) {
        if (!matchesValueKind(member) && tryMember(member)) {
          return true;
        }
      }
      return ThrowConversionError(cx, "union");
    }

    case WebIDLTypeKind::BufferSource: {
      // Per WebIDL, a buffer source is accepted by identity only: the IDL value
      // holds the very object the caller passed, with no copying or coercion.
      if (!value.isObject() ||
          !MatchesBufferSourceKind(&value.toObject(),
                                   type.bufferSourceKind())) {
        return ThrowConversionError(cx, "buffer source");
      }
      if (!type.hasAllowShared() &&
          IsSharedBufferSource(&value.toObject(), type.bufferSourceKind())) {
        return ThrowConversionError(cx, "shared buffer source");
      }
      out.set(WebIDLValue::makeObject(type, &value.toObject()));
      return true;
    }

    case WebIDLTypeKind::Record:
    case WebIDLTypeKind::Callback:
    case WebIDLTypeKind::Symbol:
    case WebIDLTypeKind::Promise:
    case WebIDLTypeKind::ReadableStream:
      // TODO(webidl): implement these conversions.
      return ThrowConversionError(cx, "unimplemented WebIDL type");

    case WebIDLTypeKind::Invalid:
    case WebIDLTypeKind::Limit:
      break;
  }
  MOZ_CRASH("bad WebIDL type kind");
}

JS_PUBLIC_API bool JS::JSToWebIDLValue(JSContext* cx, HandleValue value,
                                       const WebIDLType& type,
                                       MutableHandleWebIDLValue out) {
  return JSToWebIDLValueImpl(cx, value, type, out);
}

static bool WebIDLValueToJSImpl(JSContext* cx, HandleWebIDLValue value,
                                MutableHandleValue out);

JS_PUBLIC_API bool JS::WebIDLValueToJS(JSContext* cx, HandleWebIDLValue value,
                                       MutableHandleValue out) {
  return WebIDLValueToJSImpl(cx, value, out);
}

static bool WebIDLValueToJSImpl(JSContext* cx, HandleWebIDLValue value,
                                MutableHandleValue out) {
  const WebIDLType& type = value.get().type();
  switch (type.kind()) {
    case WebIDLTypeKind::Any:
      out.set(value.get().asAny());
      return true;
    case WebIDLTypeKind::Undefined:
      out.setUndefined();
      return true;
    case WebIDLTypeKind::Boolean:
      out.setBoolean(value.get().asBool());
      return true;
    case WebIDLTypeKind::Byte:
    case WebIDLTypeKind::Short:
    case WebIDLTypeKind::Long:
    case WebIDLTypeKind::LongLong:
      out.set(JS::NumberValue(double(value.get().asSigned())));
      return true;
    case WebIDLTypeKind::Octet:
    case WebIDLTypeKind::UnsignedShort:
    case WebIDLTypeKind::UnsignedLong:
    case WebIDLTypeKind::UnsignedLongLong:
      out.set(JS::NumberValue(double(value.get().asUnsigned())));
      return true;
    case WebIDLTypeKind::Float:
    case WebIDLTypeKind::UnrestrictedFloat:
    case WebIDLTypeKind::Double:
    case WebIDLTypeKind::UnrestrictedDouble:
      out.set(JS::NumberValue(value.get().asDouble()));
      return true;
    case WebIDLTypeKind::BigInt:
      out.setBigInt(value.get().asBigInt());
      return true;
    case WebIDLTypeKind::DOMString:
    case WebIDLTypeKind::USVString:
    case WebIDLTypeKind::ByteString:
      out.setString(value.get().asString());
      return true;
    case WebIDLTypeKind::Object:
    case WebIDLTypeKind::Interface:
      out.setObject(*value.get().asObject());
      return true;
    case WebIDLTypeKind::Enumeration: {
      const WebIDLEnumerationType& en =
          type.def()->schema().as<WebIDLEnumerationType>();
      const char* label = en.labels[value.get().asEnumIndex()].get();
      JSString* str = JS_NewStringCopyZ(cx, label);
      if (!str) {
        return false;
      }
      out.setString(str);
      return true;
    }
    case WebIDLTypeKind::Nullable:
      // A non-null nullable value is stored with its inner type, so a value
      // still typed Nullable here is always null.
      MOZ_ASSERT(value.get().isNull());
      out.setNull();
      return true;
    case WebIDLTypeKind::Sequence:
    case WebIDLTypeKind::FrozenArray:
    case WebIDLTypeKind::ObservableArray: {
      const WebIDLValueData::SequenceData& seq =
          value.get().data()->asSequence();
      RootedObject array(cx, JS::NewArrayObject(cx, seq.elements.length()));
      if (!array) {
        return false;
      }
      RootedWebIDLValue elem(cx);
      RootedValue elemJS(cx);
      for (size_t i = 0; i < seq.elements.length(); i++) {
        elem = seq.elements[i];
        if (!WebIDLValueToJSImpl(cx, elem, &elemJS)) {
          return false;
        }
        if (!JS_SetElement(cx, array, i, elemJS)) {
          return false;
        }
      }
      out.setObject(*array);
      return true;
    }
    case WebIDLTypeKind::Dictionary: {
      const WebIDLDictionaryType& dict =
          type.def()->schema().as<WebIDLDictionaryType>();
      const WebIDLValueData::DictionaryData& data =
          value.get().data()->asDictionary();
      RootedObject obj(cx, JS_NewPlainObject(cx));
      if (!obj) {
        return false;
      }
      RootedWebIDLValue member(cx);
      RootedValue memberJS(cx);
      for (size_t i = 0; i < dict.members.length(); i++) {
        // Absent optional members are stored as default (Invalid) placeholders.
        if (data.members[i].type().kind() == WebIDLTypeKind::Invalid) {
          continue;
        }
        member = data.members[i];
        if (!WebIDLValueToJSImpl(cx, member, &memberJS)) {
          return false;
        }
        if (!JS_SetProperty(cx, obj, dict.members[i].name.get(), memberJS)) {
          return false;
        }
      }
      out.setObject(*obj);
      return true;
    }
    case WebIDLTypeKind::BufferSource:
      // Held by identity (see JSToWebIDLValueImpl), so this hands back the very
      // object that came in.
      out.setObject(*value.get().asObject());
      return true;

    case WebIDLTypeKind::Union:
    case WebIDLTypeKind::Record:
    case WebIDLTypeKind::Callback:
    case WebIDLTypeKind::Symbol:
    case WebIDLTypeKind::Promise:
    case WebIDLTypeKind::ReadableStream:
      // TODO(webidl): implement these conversions. A union value is stored
      // with its resolved member type, so `Union` should not appear here.
      JS_ReportErrorASCII(cx, "WebIDLValueToJS: unimplemented type");
      return false;
    case WebIDLTypeKind::Invalid:
    case WebIDLTypeKind::Limit:
      break;
  }
  MOZ_CRASH("bad WebIDL type kind");
}

// An `undefined` source is treated as NaN, matching `ToNumber(undefined)`.
static bool NumericSourceAsDouble(const WebIDLValue& v, double* out) {
  const WebIDLType& t = v.type();
  if (t.kind() == WebIDLTypeKind::Undefined) {
    *out = std::nan("");
    return true;
  }
  if (WebIDLTypeKindIsFloat(t.kind())) {
    *out = v.asDouble();
    return true;
  }
  if (WebIDLTypeKindIsInteger(t.kind())) {
    int bits;
    bool isSigned;
    IntegerWidthAndSign(t.kind(), &bits, &isSigned);
    *out = isSigned ? double(v.asSigned()) : double(v.asUnsigned());
    return true;
  }
  return false;
}

// Copies a byte-typed buffer source's bytes out into `out`. Separated from its
// callers because building anything on the GC heap invalidates the data
// pointer, so the copy has to complete inside one no-GC region.
static bool CopyBufferSourceBytes(JSContext* cx, JSObject* obj,
                                  mozilla::Vector<uint8_t>* out) {
  JSObject* unwrapped =
      JS::ArrayBufferOrView::unwrap(obj).asObjectUnbarriered();
  if (!unwrapped || JS::ArrayBufferOrView::fromObject(unwrapped).isDetached()) {
    return ThrowConversionError(cx, "buffer source");
  }
  bool isBuffer = !!JS::ArrayBuffer::fromObject(unwrapped);
  size_t len = isBuffer ? JS::GetArrayBufferByteLength(unwrapped)
                        : JS_GetArrayBufferViewByteLength(unwrapped);
  if (!out->resizeUninitialized(len)) {
    js::ReportOutOfMemory(cx);
    return false;
  }
  if (len == 0) {
    return true;
  }
  JS::AutoCheckCannotGC nogc;
  bool isShared;
  mozilla::Span<uint8_t> src =
      isBuffer
          ? JS::ArrayBuffer::fromObject(unwrapped).getData(&isShared, nogc)
          : JS::ArrayBufferView::fromObject(unwrapped).getData(&isShared, nogc);
  MOZ_RELEASE_ASSERT(src.Length() == len);
  memcpy(out->begin(), src.Elements(), len);
  return true;
}

// A byte-typed buffer source coerced to `sequence<T>`: one element per byte,
// each recursively coerced to the target's element type.
static bool CoerceBufferSourceToSequence(JSContext* cx, HandleWebIDLValue from,
                                         const WebIDLType& to,
                                         MutableHandleWebIDLValue out) {
  if (!WebIDLBufferSourceKindIsBytes(from.get().type().bufferSourceKind())) {
    return ThrowConversionError(cx, "non-byte buffer source to sequence");
  }
  mozilla::Vector<uint8_t> bytes;
  if (!CopyBufferSourceBytes(cx, from.get().asObject(), &bytes)) {
    return false;
  }

  RefPtr<WebIDLValueData> data =
      js_new<WebIDLValueData>(WebIDLValueData::SequenceData{});
  if (!data) {
    js::ReportOutOfMemory(cx);
    return false;
  }
  out.set(WebIDLValue::makeComposite(to, data));

  WebIDLType octet = WebIDLType::primitive(WebIDLTypeKind::Octet);
  WebIDLType elementType = to.innerType();
  RootedWebIDLValue srcElem(cx);
  RootedWebIDLValue coercedElem(cx);
  for (uint8_t b : bytes) {
    srcElem = WebIDLValue::makeUnsigned(octet, b);
    if (!JS::CoerceWebIDLValue(cx, srcElem, elementType, &coercedElem) ||
        !data->appendSequenceElement(coercedElem.get())) {
      return false;
    }
  }
  return true;
}

// A `sequence<T>` coerced to a byte-typed buffer source: each element is
// coerced to `octet`, then the whole run is copied into a fresh buffer source
// of the target kind.
static bool CoerceSequenceToBufferSource(JSContext* cx, HandleWebIDLValue from,
                                         const WebIDLType& to,
                                         MutableHandleWebIDLValue out) {
  if (!WebIDLBufferSourceKindIsBytes(to.bufferSourceKind())) {
    return ThrowConversionError(cx, "sequence to non-byte buffer source");
  }
  const WebIDLValueData::SequenceData& srcSeq = from.get().data()->asSequence();

  WebIDLType octet = WebIDLType::primitive(WebIDLTypeKind::Octet);
  mozilla::Vector<uint8_t> bytes;
  if (!bytes.reserve(srcSeq.elements.length())) {
    js::ReportOutOfMemory(cx);
    return false;
  }
  RootedWebIDLValue srcElem(cx);
  RootedWebIDLValue coercedElem(cx);
  for (const WebIDLValue& elem : srcSeq.elements) {
    srcElem = elem;
    if (!JS::CoerceWebIDLValue(cx, srcElem, octet, &coercedElem)) {
      return false;
    }
    bytes.infallibleAppend(uint8_t(coercedElem.get().asUnsigned()));
  }

  JS::Rooted<JSObject*> obj(cx);
  switch (to.bufferSourceKind()) {
    case WebIDLBufferSourceKind::ArrayBuffer:
      obj = JS::NewArrayBuffer(cx, bytes.length());
      break;
    case WebIDLBufferSourceKind::Uint8Array:
      obj = JS_NewUint8Array(cx, bytes.length());
      break;
    case WebIDLBufferSourceKind::Uint8ClampedArray:
      obj = JS_NewUint8ClampedArray(cx, bytes.length());
      break;
    default:
      MOZ_CRASH("not a byte-typed buffer source kind");
  }
  if (!obj) {
    return false;
  }
  if (!bytes.empty()) {
    JS::AutoCheckCannotGC nogc;
    bool isShared;
    mozilla::Span<uint8_t> dest =
        to.bufferSourceKind() == WebIDLBufferSourceKind::ArrayBuffer
            ? JS::ArrayBuffer::fromObject(obj).getData(&isShared, nogc)
            : JS::ArrayBufferView::fromObject(obj).getData(&isShared, nogc);
    MOZ_RELEASE_ASSERT(dest.Length() == bytes.length());
    memcpy(dest.Elements(), bytes.begin(), bytes.length());
  }
  out.set(WebIDLValue::makeObject(to, obj));
  return true;
}

JS_PUBLIC_API bool JS::CoerceWebIDLValue(JSContext* cx, HandleWebIDLValue from,
                                         const WebIDLType& to,
                                         MutableHandleWebIDLValue out) {
  const WebIDLType& src = from.get().type();

  switch (to.kind()) {
    case WebIDLTypeKind::Any:
      out.set(from.get());
      return true;

    case WebIDLTypeKind::Undefined:
      if (src.kind() != WebIDLTypeKind::Undefined) {
        return ThrowConversionError(cx, "undefined");
      }
      out.set(WebIDLValue::makeUndefined(to));
      return true;

    case WebIDLTypeKind::Boolean:
      if (src.kind() == WebIDLTypeKind::Undefined) {
        // Matches ToBoolean(undefined).
        out.set(WebIDLValue::makeBool(to, false));
        return true;
      }
      if (src.kind() != WebIDLTypeKind::Boolean) {
        return ThrowConversionError(cx, "boolean");
      }
      out.set(WebIDLValue::makeBool(to, from.get().asBool()));
      return true;

    case WebIDLTypeKind::Byte:
    case WebIDLTypeKind::Octet:
    case WebIDLTypeKind::Short:
    case WebIDLTypeKind::UnsignedShort:
    case WebIDLTypeKind::Long:
    case WebIDLTypeKind::UnsignedLong:
    case WebIDLTypeKind::LongLong:
    case WebIDLTypeKind::UnsignedLongLong: {
      double d;
      if (!NumericSourceAsDouble(from.get(), &d)) {
        return ThrowConversionError(cx, "integer");
      }
      return StoreInteger(cx, d, to, out);
    }

    case WebIDLTypeKind::Float:
    case WebIDLTypeKind::UnrestrictedFloat:
    case WebIDLTypeKind::Double:
    case WebIDLTypeKind::UnrestrictedDouble: {
      double d;
      if (!NumericSourceAsDouble(from.get(), &d)) {
        return ThrowConversionError(cx, "float");
      }
      return StoreFloat(cx, d, to, out);
    }

    case WebIDLTypeKind::DOMString:
    case WebIDLTypeKind::USVString:
    case WebIDLTypeKind::ByteString: {
      // An `undefined` source produces "undefined", matching
      // ToString(undefined).
      if (!WebIDLTypeKindIsString(src.kind()) &&
          src.kind() != WebIDLTypeKind::Enumeration &&
          src.kind() != WebIDLTypeKind::Undefined) {
        return ThrowConversionError(cx, "string");
      }
      RootedString str(cx);
      if (src.kind() == WebIDLTypeKind::Undefined) {
        str = JS_NewStringCopyZ(cx, "undefined");
        if (!str) {
          return false;
        }
      } else if (src.kind() == WebIDLTypeKind::Enumeration) {
        const WebIDLEnumerationType& en =
            src.def()->schema().as<WebIDLEnumerationType>();
        str = JS_NewStringCopyZ(cx, en.labels[from.get().asEnumIndex()].get());
        if (!str) {
          return false;
        }
      } else {
        str = from.get().asString();
      }
      if (to.kind() == WebIDLTypeKind::USVString) {
        str = NormalizeToUSV(cx, str);
        if (!str) {
          return false;
        }
      } else if (to.kind() == WebIDLTypeKind::ByteString) {
        if (!CheckByteString(cx, str)) {
          return false;
        }
      }
      out.set(WebIDLValue::makeString(to, str));
      return true;
    }

    case WebIDLTypeKind::Object:
      if (!from.get().isObjectRepr()) {
        return ThrowConversionError(cx, "object");
      }
      out.set(WebIDLValue::makeObject(to, from.get().asObject()));
      return true;

    case WebIDLTypeKind::Interface: {
      if (src.kind() != WebIDLTypeKind::Interface) {
        return ThrowConversionError(cx, "interface");
      }
      if (!src.interface()->isOrDerivedFrom(to.interface())) {
        return ThrowConversionError(cx, "interface (not derived)");
      }
      out.set(WebIDLValue::makeObject(to, from.get().asObject()));
      return true;
    }

    case WebIDLTypeKind::Nullable:
      if (from.get().isNothing()) {
        out.set(WebIDLValue::makeNull(to));
        return true;
      }
      return JS::CoerceWebIDLValue(cx, from, to.innerType(), out);

    case WebIDLTypeKind::Union: {
      const WebIDLUnionType& un = to.def()->schema().as<WebIDLUnionType>();
      for (const WebIDLType& member : un.members) {
        if (JS::CoerceWebIDLValue(cx, from, member, out)) {
          return true;
        }
        if (cx->isExceptionPending()) {
          cx->clearPendingException();
        }
      }
      return ThrowConversionError(cx, "union");
    }

    case WebIDLTypeKind::Enumeration: {
      RootedString str(cx);
      if (WebIDLTypeKindIsString(src.kind())) {
        str = from.get().asString();
      } else if (src.kind() == WebIDLTypeKind::Enumeration) {
        const WebIDLEnumerationType& srcEn =
            src.def()->schema().as<WebIDLEnumerationType>();
        str =
            JS_NewStringCopyZ(cx, srcEn.labels[from.get().asEnumIndex()].get());
        if (!str) {
          return false;
        }
      } else {
        return ThrowConversionError(cx, "enumeration");
      }
      JS::UniqueChars label = JS_EncodeStringToUTF8(cx, str);
      if (!label) {
        return false;
      }
      const WebIDLEnumerationType& en =
          to.def()->schema().as<WebIDLEnumerationType>();
      mozilla::Maybe<uint32_t> index =
          en.find(label.get(), std::strlen(label.get()));
      if (index.isNothing()) {
        return ThrowConversionError(cx, "invalid enumeration value");
      }
      out.set(WebIDLValue::makeEnum(to, *index));
      return true;
    }

    case WebIDLTypeKind::Sequence:
    case WebIDLTypeKind::FrozenArray:
    case WebIDLTypeKind::ObservableArray: {
      // A byte-typed buffer source coerces to a sequence element-wise. This
      // crosses the overload class boundary (both are Object), which the
      // within-kind restriction otherwise forbids - a deliberate extension so a
      // component `list<u8>` can bridge to/from a Uint8Array-typed operation.
      if (src.kind() == WebIDLTypeKind::BufferSource) {
        return CoerceBufferSourceToSequence(cx, from, to, out);
      }
      if (src.kind() != WebIDLTypeKind::Sequence &&
          src.kind() != WebIDLTypeKind::FrozenArray &&
          src.kind() != WebIDLTypeKind::ObservableArray) {
        return ThrowConversionError(cx, "sequence");
      }
      WebIDLType elementType = to.innerType();
      const WebIDLValueData::SequenceData& srcSeq =
          from.get().data()->asSequence();

      RefPtr<WebIDLValueData> data =
          js_new<WebIDLValueData>(WebIDLValueData::SequenceData{});
      if (!data) {
        js::ReportOutOfMemory(cx);
        return false;
      }
      out.set(WebIDLValue::makeComposite(to, data));

      RootedWebIDLValue srcElem(cx);
      RootedWebIDLValue coercedElem(cx);
      for (const WebIDLValue& elem : srcSeq.elements) {
        srcElem = elem;
        if (!JS::CoerceWebIDLValue(cx, srcElem, elementType, &coercedElem) ||
            !data->appendSequenceElement(coercedElem.get())) {
          return false;
        }
      }
      return true;
    }

    case WebIDLTypeKind::Dictionary: {
      // NOTE(webidl): coercing a `record` source, or building an
      // all-defaults dictionary from an `undefined` source, are not modeled
      // yet (see Web.md's Dictionary TODO).
      if (src.kind() != WebIDLTypeKind::Dictionary) {
        return ThrowConversionError(cx, "dictionary");
      }
      const WebIDLDictionaryType& srcDict =
          src.def()->schema().as<WebIDLDictionaryType>();
      const WebIDLDictionaryType& toDict =
          to.def()->schema().as<WebIDLDictionaryType>();
      const WebIDLValueData::DictionaryData& srcData =
          from.get().data()->asDictionary();

      RefPtr<WebIDLValueData> data =
          js_new<WebIDLValueData>(WebIDLValueData::DictionaryData{});
      if (!data) {
        js::ReportOutOfMemory(cx);
        return false;
      }
      out.set(WebIDLValue::makeComposite(to, data));

      RootedWebIDLValue srcMember(cx);
      RootedWebIDLValue coercedMember(cx);
      for (const WebIDLDictionaryMember& member : toDict.members) {
        mozilla::Maybe<uint32_t> srcIndex =
            FindDictionaryMemberIndex(srcDict, member.name.get());
        bool present = srcIndex && srcData.members[*srcIndex].type().kind() !=
                                       WebIDLTypeKind::Invalid;
        if (!present) {
          if (member.required) {
            return ThrowConversionError(cx,
                                        "missing required dictionary member");
          }
          if (!data->appendDictionaryMember(WebIDLValue())) {
            js::ReportOutOfMemory(cx);
            return false;
          }
          continue;
        }
        srcMember = srcData.members[*srcIndex];
        if (!JS::CoerceWebIDLValue(cx, srcMember, member.type,
                                   &coercedMember) ||
            !data->appendDictionaryMember(coercedMember.get())) {
          return false;
        }
      }
      return true;
    }

    case WebIDLTypeKind::BufferSource:
      // Identity within the same kind, per WebIDL. The sequence source is the
      // deliberate extension described in CoerceBufferSourceToSequence's
      // counterpart above.
      if (src.kind() == WebIDLTypeKind::BufferSource) {
        if (!MatchesBufferSourceKind(from.get().asObject(),
                                     to.bufferSourceKind())) {
          return ThrowConversionError(cx, "buffer source");
        }
        out.set(WebIDLValue::makeObject(to, from.get().asObject()));
        return true;
      }
      if (src.kind() == WebIDLTypeKind::Sequence ||
          src.kind() == WebIDLTypeKind::FrozenArray ||
          src.kind() == WebIDLTypeKind::ObservableArray) {
        return CoerceSequenceToBufferSource(cx, from, to, out);
      }
      return ThrowConversionError(cx, "buffer source");

    case WebIDLTypeKind::BigInt:
    case WebIDLTypeKind::Symbol:
    case WebIDLTypeKind::Callback:
    case WebIDLTypeKind::Record:
    case WebIDLTypeKind::Promise:
    case WebIDLTypeKind::ReadableStream:
      // TODO(webidl): implement these coercions.
      return ThrowConversionError(cx, "unimplemented coercion");

    case WebIDLTypeKind::Invalid:
    case WebIDLTypeKind::Limit:
      break;
  }
  MOZ_CRASH("bad WebIDL type kind");
}
