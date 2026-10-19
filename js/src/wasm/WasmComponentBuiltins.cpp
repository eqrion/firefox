/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "wasm/WasmComponentBuiltins.h"

#ifdef ENABLE_WASM_COMPONENTS

#  include "builtin/String.h"
#  include "js/Conversions.h"
#  include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#  include "util/Unicode.h"
#  include "vm/BigIntType.h"
#  include "wasm/WasmBuiltins.h"
#  include "wasm/WasmInstance.h"

#  include "vm/JSObject-inl.h"
#  include "wasm/WasmInstance-inl.h"

using namespace js;
using namespace js::wasm;

// The canonical ABI's lift_flat_signed and lift_flat_unsigned, which keep only
// the bits of the type's width.
static double LiftFlatInteger(int32_t value, ComponentTypeKind kind) {
  switch (kind) {
    case ComponentTypeKind::S8:
      return int8_t(value);
    case ComponentTypeKind::U8:
      return uint8_t(value);
    case ComponentTypeKind::S16:
      return int16_t(value);
    case ComponentTypeKind::U16:
      return uint16_t(value);
    case ComponentTypeKind::S32:
      return value;
    case ComponentTypeKind::U32:
      return uint32_t(value);
    default:
      MOZ_CRASH("not an integer type");
  }
}

// Returns a JS value as an externref for a builtin that fails with
// FailOnInvalidRef.
static void* ExternRefForCompiledCode(JSContext* cx, HandleValue value) {
  Rooted<AnyRef> result(cx);
  if (!AnyRef::fromJSValue(cx, value, &result)) {
    return AnyRef::invalid().forCompiledCode();
  }
  return result.get().forCompiledCode();
}

// Converts a JS value with one of the JS::ToInt8 family of conversions, for
// the sign- or zero-extended bits of the result.
template <typename T>
static bool ToIntegerBits(JSContext* cx, HandleValue value,
                          bool (*convert)(JSContext*, HandleValue, T*),
                          uint64_t* bits) {
  T n;
  if (!convert(cx, value, &n)) {
    return false;
  }
  *bits = uint64_t(n);
  return true;
}

// The JS-API's ToComponentValueInteger, for the bits of the integer.
static bool ToComponentValueInteger(JSContext* cx, HandleValue value,
                                    ComponentTypeKind kind, uint64_t* bits) {
  switch (kind) {
    case ComponentTypeKind::S8:
      return ToIntegerBits(cx, value, JS::ToInt8, bits);
    case ComponentTypeKind::U8:
      return ToIntegerBits(cx, value, JS::ToUint8, bits);
    case ComponentTypeKind::S16:
      return ToIntegerBits(cx, value, JS::ToInt16, bits);
    case ComponentTypeKind::U16:
      return ToIntegerBits(cx, value, JS::ToUint16, bits);
    case ComponentTypeKind::S32:
      return ToIntegerBits(cx, value, JS::ToInt32, bits);
    case ComponentTypeKind::U32:
      return ToIntegerBits(cx, value, JS::ToUint32, bits);
    case ComponentTypeKind::S64: {
      int64_t n;
      JS_TRY_VAR_OR_RETURN_FALSE(cx, n, ToBigInt64(cx, value));
      *bits = uint64_t(n);
      return true;
    }
    case ComponentTypeKind::U64:
      JS_TRY_VAR_OR_RETURN_FALSE(cx, *bits, ToBigUint64(cx, value));
      return true;
    default:
      MOZ_CRASH("not an integer type");
  }
}

int64_t wasm::ComponentToInteger(Instance* instance, void* valueRef,
                                 int32_t kind) {
  MOZ_ASSERT(SASigComponentToInteger.failureMode ==
             FailureMode::FailOnInstanceFlag);
  JSContext* cx = instance->cx();
  RootedValue value(cx, AnyRef::fromCompiledCode(valueRef).toJSValue());
  uint64_t bits;
  if (!ToComponentValueInteger(cx, value, ComponentTypeKind(kind), &bits)) {
    return instance->failBuiltin<int64_t>();
  }
  return int64_t(bits);
}

double wasm::ComponentToFloat(Instance* instance, void* valueRef) {
  MOZ_ASSERT(SASigComponentToFloat.failureMode ==
             FailureMode::FailOnInstanceFlag);
  JSContext* cx = instance->cx();
  RootedValue value(cx, AnyRef::fromCompiledCode(valueRef).toJSValue());
  double number;
  if (!ToNumber(cx, value, &number)) {
    return instance->failBuiltin<double>();
  }
  return number;
}

int32_t wasm::ComponentToBool(Instance* instance, void* valueRef) {
  MOZ_ASSERT(SASigComponentToBool.failureMode == FailureMode::Infallible);
  JSContext* cx = instance->cx();
  RootedValue value(cx, AnyRef::fromCompiledCode(valueRef).toJSValue());
  return ToBoolean(value);
}

int32_t wasm::ComponentToChar(Instance* instance, void* valueRef) {
  MOZ_ASSERT(SASigComponentToChar.failureMode == FailureMode::FailOnNegI32);
  JSContext* cx = instance->cx();
  RootedValue value(cx, AnyRef::fromCompiledCode(valueRef).toJSValue());
  JSString* string = ToString(cx, value);
  JSLinearString* str = string ? string->ensureLinear(cx) : nullptr;
  if (!str) {
    return -1;
  }

  // The string must be exactly one scalar value.
  char32_t codePoint;
  size_t length = str->length();
  if (length == 1 && !unicode::IsSurrogate(str->latin1OrTwoByteChar(0))) {
    codePoint = str->latin1OrTwoByteChar(0);
  } else if (length == 2 &&
             unicode::IsLeadSurrogate(str->latin1OrTwoByteChar(0)) &&
             unicode::IsTrailSurrogate(str->latin1OrTwoByteChar(1))) {
    codePoint = unicode::UTF16Decode(str->latin1OrTwoByteChar(0),
                                     str->latin1OrTwoByteChar(1));
  } else {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_WASM_COMPONENT_BAD_CHAR);
    return -1;
  }
  return int32_t(codePoint);
}

void* wasm::ComponentBoolToJS(Instance* instance, int32_t value) {
  MOZ_ASSERT(SASigComponentBoolToJS.failureMode ==
             FailureMode::FailOnInvalidRef);
  JSContext* cx = instance->cx();
  RootedValue jsValue(cx, BooleanValue(value != 0));
  return ExternRefForCompiledCode(cx, jsValue);
}

void* wasm::ComponentIntegerToJS(Instance* instance, int32_t value,
                                 int32_t kind) {
  MOZ_ASSERT(SASigComponentIntegerToJS.failureMode ==
             FailureMode::FailOnInvalidRef);
  JSContext* cx = instance->cx();
  RootedValue jsValue(
      cx, NumberValue(LiftFlatInteger(value, ComponentTypeKind(kind))));
  return ExternRefForCompiledCode(cx, jsValue);
}

void* wasm::ComponentInt64ToJS(Instance* instance, int64_t value,
                               int32_t kind) {
  MOZ_ASSERT(SASigComponentInt64ToJS.failureMode ==
             FailureMode::FailOnInvalidRef);
  JSContext* cx = instance->cx();
  BigInt* bigInt = ComponentTypeKind(kind) == ComponentTypeKind::S64
                       ? BigInt::createFromInt64(cx, value)
                       : BigInt::createFromUint64(cx, uint64_t(value));
  if (!bigInt) {
    return AnyRef::invalid().forCompiledCode();
  }
  RootedValue jsValue(cx, BigIntValue(bigInt));
  return ExternRefForCompiledCode(cx, jsValue);
}

void* wasm::ComponentFloatToJS(Instance* instance, double value) {
  MOZ_ASSERT(SASigComponentFloatToJS.failureMode ==
             FailureMode::FailOnInvalidRef);
  JSContext* cx = instance->cx();
  RootedValue jsValue(cx, NumberValue(value));
  return ExternRefForCompiledCode(cx, jsValue);
}

void* wasm::ComponentCharToJS(Instance* instance, int32_t value) {
  MOZ_ASSERT(SASigComponentCharToJS.failureMode ==
             FailureMode::FailOnInvalidRef);
  JSContext* cx = instance->cx();
  uint32_t codePoint = uint32_t(value);
  if (codePoint > unicode::NonBMPMax || unicode::IsSurrogate(codePoint)) {
    ReportTrapError(cx, JSMSG_WASM_BAD_CODEPOINT);
    return AnyRef::invalid().forCompiledCode();
  }
  JSLinearString* str = StringFromCodePoint(cx, char32_t(codePoint));
  if (!str) {
    return AnyRef::invalid().forCompiledCode();
  }
  RootedValue jsValue(cx, StringValue(str));
  return ExternRefForCompiledCode(cx, jsValue);
}

int32_t wasm::ComponentTrap(Instance* instance, int32_t reason) {
  MOZ_ASSERT(SASigComponentTrap.failureMode == FailureMode::FailOnNegI32);
  switch (AdapterTrap(reason)) {
    case AdapterTrap::UncaughtException:
      ReportTrapError(instance->cx(), JSMSG_WASM_COMPONENT_UNCAUGHT_EXCEPTION);
      break;
    case AdapterTrap::ImportException:
      ReportTrapError(instance->cx(), JSMSG_WASM_COMPONENT_IMPORT_EXCEPTION);
      break;
  }
  return -1;
}

#endif  // ENABLE_WASM_COMPONENTS
