/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "wasm/WasmComponentBuiltins.h"

#ifdef ENABLE_WASM_COMPONENTS

#  include "mozilla/Casting.h"

#  include <cmath>

#  include "jsfriendapi.h"

#  include "builtin/MapObject.h"
#  include "builtin/String.h"
#  include "js/CallAndConstruct.h"
#  include "js/Conversions.h"
#  include "js/experimental/TypedData.h"
#  include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#  include "util/Unicode.h"
#  include "vm/BigIntType.h"
#  include "vm/Interpreter.h"
#  include "vm/Iteration.h"
#  include "vm/TypedArrayObject.h"
#  include "wasm/WasmBuiltins.h"
#  include "wasm/WasmInstance.h"
#  include "wasm/WasmJS.h"

#  include "vm/JSObject-inl.h"
#  include "wasm/WasmInstance-inl.h"

using namespace js;
using namespace js::wasm;

// The canonical ABI's canonical NaNs.
static constexpr uint32_t CanonicalFloat32NaN = 0x7fc00000;
static constexpr uint64_t CanonicalFloat64NaN = 0x7ff8000000000000;

mozilla::Maybe<Scalar::Type> wasm::ScalarListArrayType(ComponentTypeKind kind) {
  switch (kind) {
    case ComponentTypeKind::S8:
      return mozilla::Some(Scalar::Int8);
    case ComponentTypeKind::U8:
      return mozilla::Some(Scalar::Uint8);
    case ComponentTypeKind::S16:
      return mozilla::Some(Scalar::Int16);
    case ComponentTypeKind::U16:
      return mozilla::Some(Scalar::Uint16);
    case ComponentTypeKind::S32:
      return mozilla::Some(Scalar::Int32);
    case ComponentTypeKind::U32:
      return mozilla::Some(Scalar::Uint32);
    case ComponentTypeKind::S64:
      return mozilla::Some(Scalar::BigInt64);
    case ComponentTypeKind::U64:
      return mozilla::Some(Scalar::BigUint64);
    case ComponentTypeKind::F32:
      return mozilla::Some(Scalar::Float32);
    case ComponentTypeKind::F64:
      return mozilla::Some(Scalar::Float64);
    default:
      return mozilla::Nothing();
  }
}

static ComponentInstance& InstanceFromCtx(HandleObject ctx) {
  return ctx->as<WasmComponentInstanceObject>().instance();
}

static ComponentInstance& InstanceFromCtxRef(void* ctxRef) {
  return AnyRef::fromCompiledCode(ctxRef)
      .toJSObject()
      .as<WasmComponentInstanceObject>()
      .instance();
}

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

void wasm::ComponentSetError(Instance* instance, void* ctxRef, void* dataRef,
                             int32_t hasData) {
  MOZ_ASSERT(SASigComponentSetError.failureMode == FailureMode::Infallible);
  JSContext* cx = instance->cx();
  RootedObject ctx(cx, &AnyRef::fromCompiledCode(ctxRef).toJSObject());
  InstanceFromCtx(ctx).setPendingError(
      hasData ? AnyRef::fromCompiledCode(dataRef).toJSValue()
              : UndefinedValue());
}

void* wasm::ComponentExceptionValue(Instance* instance, void* exnRef) {
  MOZ_ASSERT(SASigComponentExceptionValue.failureMode ==
             FailureMode::FailOnInvalidRef);
  JSContext* cx = instance->cx();
  RootedValue value(cx, AnyRef::fromCompiledCode(exnRef)
                            .toJSObject()
                            .as<WasmExceptionObject>()
                            .toJSValue());
  return ExternRefForCompiledCode(cx, value);
}

void wasm::ComponentEnterGuest(Instance* instance, void* ctxRef) {
  MOZ_ASSERT(SASigComponentEnterGuest.failureMode == FailureMode::Infallible);
  JSContext* cx = instance->cx();
  RootedObject ctx(cx, &AnyRef::fromCompiledCode(ctxRef).toJSObject());
  InstanceFromCtx(ctx).setEnteredGuest(true);
}

// Gets [ptr, ptr + length) in memory 0, or traps if it is out of bounds.
static uint8_t* GetMemoryRange(Instance* instance, uint32_t ptr,
                               uint64_t length) {
  uint64_t memoryLength = instance->memory(0)->volatileMemoryLength();
  if (uint64_t(ptr) + length > memoryLength) {
    ReportTrapError(instance->cx(), JSMSG_WASM_OUT_OF_BOUNDS);
    return nullptr;
  }
  return instance->memoryBase(0).unwrap() + ptr;
}

static bool CheckAlignment(Instance* instance, uint32_t ptr,
                           uint32_t alignment) {
  if (ptr % alignment != 0) {
    ReportTrapError(instance->cx(), JSMSG_WASM_UNALIGNED_ACCESS);
    return false;
  }
  return true;
}

int32_t wasm::ComponentCheckRange(Instance* instance, int32_t ptr,
                                  int32_t count, int32_t elemSize,
                                  int32_t alignment) {
  MOZ_ASSERT(SASigComponentCheckRange.failureMode == FailureMode::FailOnNegI32);
  if (!CheckAlignment(instance, uint32_t(ptr), uint32_t(alignment)) ||
      !GetMemoryRange(instance, uint32_t(ptr),
                      uint64_t(uint32_t(count)) * uint32_t(elemSize))) {
    return -1;
  }
  return 0;
}

// Returns the array of a list's elements, or of a map's entries, as an
// externref for a builtin that fails with FailOnInvalidRef. Throws if its size
// in bytes doesn't fit in a 32-bit memory.
static void* ListArrayRef(JSContext* cx, Handle<ArrayObject*> array,
                          uint32_t elemSize) {
  if (!array) {
    return AnyRef::invalid().forCompiledCode();
  }
  if (uint64_t(array->length()) * elemSize > UINT32_MAX) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_WASM_COMPONENT_LIST_TOO_LONG);
    return AnyRef::invalid().forCompiledCode();
  }
  RootedValue arrayValue(cx, ObjectValue(*array));
  return ExternRefForCompiledCode(cx, arrayValue);
}

// The body of ComponentIterableToArray.
static void* IterableToArrayRef(JSContext* cx, HandleValue value,
                                uint32_t elemSize) {
  if (!value.isObject()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_NOT_ITERABLE,
                              "value");
    return AnyRef::invalid().forCompiledCode();
  }
  Rooted<ArrayObject*> array(cx, IterableToArray(cx, value));
  return ListArrayRef(cx, array, elemSize);
}

void* wasm::ComponentIterableToArray(Instance* instance, void* valueRef,
                                     int32_t elemSize) {
  MOZ_ASSERT(SASigComponentIterableToArray.failureMode ==
             FailureMode::FailOnInvalidRef);
  JSContext* cx = instance->cx();
  RootedValue value(cx, AnyRef::fromCompiledCode(valueRef).toJSValue());
  return IterableToArrayRef(cx, value, uint32_t(elemSize));
}

static ArrayObject& ArrayFromRef(void* arrayRef) {
  return AnyRef::fromCompiledCode(arrayRef).toJSObject().as<ArrayObject>();
}

int32_t wasm::ComponentArrayLength(Instance* instance, void* arrayRef) {
  MOZ_ASSERT(SASigComponentArrayLength.failureMode == FailureMode::Infallible);
  return int32_t(ArrayFromRef(arrayRef).length());
}

int32_t wasm::ComponentCheckArrayLength(Instance* instance, void* arrayRef,
                                        int32_t length) {
  MOZ_ASSERT(SASigComponentCheckArrayLength.failureMode ==
             FailureMode::FailOnNegI32);
  if (ArrayFromRef(arrayRef).length() != uint32_t(length)) {
    JS_ReportErrorNumberASCII(instance->cx(), GetErrorMessage, nullptr,
                              JSMSG_WASM_COMPONENT_BAD_LENGTH);
    return -1;
  }
  return 0;
}

void* wasm::ComponentArrayGet(Instance* instance, void* arrayRef,
                              int32_t index) {
  MOZ_ASSERT(SASigComponentArrayGet.failureMode ==
             FailureMode::FailOnInvalidRef);
  JSContext* cx = instance->cx();
  // The array is from ComponentIterableToArray, so its elements are dense.
  RootedValue value(cx,
                    ArrayFromRef(arrayRef).getDenseElement(uint32_t(index)));
  return ExternRefForCompiledCode(cx, value);
}

void* wasm::ComponentNewArray(Instance* instance, int32_t length) {
  MOZ_ASSERT(SASigComponentNewArray.failureMode ==
             FailureMode::FailOnInvalidRef);
  JSContext* cx = instance->cx();
  ArrayObject* array = NewDenseFullyAllocatedArray(cx, uint32_t(length));
  if (!array) {
    return AnyRef::invalid().forCompiledCode();
  }
  RootedValue value(cx, ObjectValue(*array));
  return ExternRefForCompiledCode(cx, value);
}

int32_t wasm::ComponentArraySet(Instance* instance, void* arrayRef,
                                int32_t index, void* valueRef) {
  MOZ_ASSERT(SASigComponentArraySet.failureMode == FailureMode::FailOnNegI32);
  JSContext* cx = instance->cx();
  Rooted<ArrayObject*> array(cx, &ArrayFromRef(arrayRef));
  RootedValue value(cx, AnyRef::fromCompiledCode(valueRef).toJSValue());
  if (!DefineDataElement(cx, array, uint32_t(index), value)) {
    return -1;
  }
  return 0;
}

int32_t wasm::ComponentCheckObject(Instance* instance, void* valueRef) {
  MOZ_ASSERT(SASigComponentCheckObject.failureMode ==
             FailureMode::FailOnNegI32);
  if (!AnyRef::fromCompiledCode(valueRef).toJSValue().isObject()) {
    JS_ReportErrorNumberASCII(instance->cx(), GetErrorMessage, nullptr,
                              JSMSG_OBJECT_REQUIRED, "value");
    return -1;
  }
  return 0;
}

void* wasm::ComponentGetField(Instance* instance, void* objectRef,
                              void* nameRef) {
  MOZ_ASSERT(SASigComponentGetField.failureMode ==
             FailureMode::FailOnInvalidRef);
  JSContext* cx = instance->cx();
  RootedObject obj(cx, &AnyRef::fromCompiledCode(objectRef).toJSObject());
  RootedValue nameValue(cx, AnyRef::fromCompiledCode(nameRef).toJSValue());
  RootedId name(cx);
  RootedValue value(cx);
  if (!ToPropertyKey(cx, nameValue, &name) ||
      !GetProperty(cx, obj, obj, name, &value)) {
    return AnyRef::invalid().forCompiledCode();
  }
  return ExternRefForCompiledCode(cx, value);
}

void* wasm::ComponentNewRecord(Instance* instance) {
  MOZ_ASSERT(SASigComponentNewRecord.failureMode ==
             FailureMode::FailOnInvalidRef);
  JSContext* cx = instance->cx();
  JSObject* obj = NewPlainObjectWithProto(cx, nullptr);
  if (!obj) {
    return AnyRef::invalid().forCompiledCode();
  }
  RootedValue value(cx, ObjectValue(*obj));
  return ExternRefForCompiledCode(cx, value);
}

int32_t wasm::ComponentDefineField(Instance* instance, void* objectRef,
                                   void* nameRef, void* valueRef) {
  MOZ_ASSERT(SASigComponentDefineField.failureMode ==
             FailureMode::FailOnNegI32);
  JSContext* cx = instance->cx();
  RootedObject obj(cx, &AnyRef::fromCompiledCode(objectRef).toJSObject());
  RootedValue nameValue(cx, AnyRef::fromCompiledCode(nameRef).toJSValue());
  RootedValue value(cx, AnyRef::fromCompiledCode(valueRef).toJSValue());
  RootedId name(cx);
  if (!ToPropertyKey(cx, nameValue, &name) ||
      !DefineDataProperty(cx, obj, name, value, JSPROP_ENUMERATE)) {
    return -1;
  }
  return 0;
}

void* wasm::ComponentNewMap(Instance* instance) {
  MOZ_ASSERT(SASigComponentNewMap.failureMode == FailureMode::FailOnInvalidRef);
  JSContext* cx = instance->cx();
  JSObject* map = MapObject::create(cx);
  if (!map) {
    return AnyRef::invalid().forCompiledCode();
  }
  RootedValue value(cx, ObjectValue(*map));
  return ExternRefForCompiledCode(cx, value);
}

int32_t wasm::ComponentMapSet(Instance* instance, void* mapRef, void* keyRef,
                              void* valueRef) {
  MOZ_ASSERT(SASigComponentMapSet.failureMode == FailureMode::FailOnNegI32);
  JSContext* cx = instance->cx();
  Rooted<MapObject*> map(
      cx, &AnyRef::fromCompiledCode(mapRef).toJSObject().as<MapObject>());
  RootedValue key(cx, AnyRef::fromCompiledCode(keyRef).toJSValue());
  RootedValue value(cx, AnyRef::fromCompiledCode(valueRef).toJSValue());
  if (!map->set(cx, key, value)) {
    return -1;
  }
  return 0;
}

// The spec's GetIteratorFromMethod and IteratorToList.
static ArrayObject* IterateToArray(JSContext* cx, HandleObject obj,
                                   HandleValue method) {
  RootedValue iterator(cx);
  if (!Call(cx, method, obj, &iterator)) {
    return nullptr;
  }
  if (!iterator.isObject()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_OBJECT_REQUIRED, "iterator");
    return nullptr;
  }
  RootedObject iteratorObj(cx, &iterator.toObject());
  RootedValue next(cx);
  if (!GetProperty(cx, iteratorObj, iteratorObj, cx->names().next, &next)) {
    return nullptr;
  }

  Rooted<ArrayObject*> array(cx, NewDenseEmptyArray(cx));
  if (!array) {
    return nullptr;
  }
  RootedValue result(cx);
  RootedValue value(cx);
  while (true) {
    if (!Call(cx, next, iterator, &result)) {
      return nullptr;
    }
    if (!result.isObject()) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_OBJECT_REQUIRED, "iterator result");
      return nullptr;
    }
    RootedObject resultObj(cx, &result.toObject());
    if (!GetProperty(cx, resultObj, resultObj, cx->names().done, &value)) {
      return nullptr;
    }
    if (ToBoolean(value)) {
      return array;
    }
    if (!GetProperty(cx, resultObj, resultObj, cx->names().value, &value) ||
        !NewbornArrayPush(cx, array, value)) {
      return nullptr;
    }
  }
}

void* wasm::ComponentGetIteratorMethod(Instance* instance, void* valueRef) {
  MOZ_ASSERT(SASigComponentGetIteratorMethod.failureMode ==
             FailureMode::FailOnInvalidRef);
  JSContext* cx = instance->cx();
  RootedValue value(cx, AnyRef::fromCompiledCode(valueRef).toJSValue());
  if (!value.isObject()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_OBJECT_REQUIRED, "value");
    return AnyRef::invalid().forCompiledCode();
  }
  RootedObject obj(cx, &value.toObject());
  RootedValue method(cx);
  RootedId iteratorId(cx, PropertyKey::Symbol(cx->wellKnownSymbols().iterator));
  if (!GetProperty(cx, obj, obj, iteratorId, &method)) {
    return AnyRef::invalid().forCompiledCode();
  }
  return ExternRefForCompiledCode(cx, method);
}

void* wasm::ComponentIterableEntries(Instance* instance, void* valueRef,
                                     void* methodRef, int32_t entrySize) {
  MOZ_ASSERT(SASigComponentIterableEntries.failureMode ==
             FailureMode::FailOnInvalidRef);
  JSContext* cx = instance->cx();
  RootedObject obj(cx, &AnyRef::fromCompiledCode(valueRef).toJSObject());
  RootedValue method(cx, AnyRef::fromCompiledCode(methodRef).toJSValue());
  if (!IsCallable(method)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_NOT_ITERABLE,
                              "value");
    return AnyRef::invalid().forCompiledCode();
  }
  Rooted<ArrayObject*> entries(cx, IterateToArray(cx, obj, method));
  return ListArrayRef(cx, entries, uint32_t(entrySize));
}

void* wasm::ComponentObjectKeys(Instance* instance, void* valueRef,
                                int32_t entrySize) {
  MOZ_ASSERT(SASigComponentObjectKeys.failureMode ==
             FailureMode::FailOnInvalidRef);
  JSContext* cx = instance->cx();
  RootedObject obj(cx, &AnyRef::fromCompiledCode(valueRef).toJSObject());
  RootedIdVector ids(cx);
  if (!GetPropertyKeys(cx, obj, JSITER_OWNONLY, &ids)) {
    return AnyRef::invalid().forCompiledCode();
  }
  Rooted<ArrayObject*> keys(cx, NewDenseEmptyArray(cx));
  if (!keys) {
    return AnyRef::invalid().forCompiledCode();
  }
  for (size_t i = 0; i < ids.length(); i++) {
    JSString* str = IdToString(cx, ids[i]);
    if (!str || !NewbornArrayPush(cx, keys, StringValue(str))) {
      return AnyRef::invalid().forCompiledCode();
    }
  }
  return ListArrayRef(cx, keys, uint32_t(entrySize));
}

void* wasm::ComponentNewVariant(Instance* instance, void* kindRef,
                                void* valueRef, int32_t hasValue) {
  MOZ_ASSERT(SASigComponentNewVariant.failureMode ==
             FailureMode::FailOnInvalidRef);
  JSContext* cx = instance->cx();
  RootedValue kind(cx, AnyRef::fromCompiledCode(kindRef).toJSValue());
  RootedValue value(cx, AnyRef::fromCompiledCode(valueRef).toJSValue());
  RootedObject obj(cx, NewPlainObjectWithProto(cx, nullptr));
  if (!obj ||
      !DefineDataProperty(cx, obj, cx->names().kind, kind, JSPROP_ENUMERATE) ||
      (hasValue && !DefineDataProperty(cx, obj, cx->names().value, value,
                                       JSPROP_ENUMERATE))) {
    return AnyRef::invalid().forCompiledCode();
  }
  RootedValue objValue(cx, ObjectValue(*obj));
  return ExternRefForCompiledCode(cx, objValue);
}

// Returns the index of the string in `names`, an array of atoms, or throws.
static int32_t FindCase(JSContext* cx, HandleString str, void* namesRef) {
  JSAtom* atom = AtomizeString(cx, str);
  if (!atom) {
    return -1;
  }
  ArrayObject& names = ArrayFromRef(namesRef);
  for (uint32_t i = 0; i < names.length(); i++) {
    if (names.getDenseElement(i).toString() == atom) {
      return int32_t(i);
    }
  }
  UniqueChars chars = JS_EncodeStringToUTF8(cx, str);
  if (chars) {
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                             JSMSG_WASM_COMPONENT_BAD_CASE, chars.get());
  }
  return -1;
}

int32_t wasm::ComponentFindVariantCase(Instance* instance, void* valueRef,
                                       void* namesRef) {
  MOZ_ASSERT(SASigComponentFindVariantCase.failureMode ==
             FailureMode::FailOnNegI32);
  JSContext* cx = instance->cx();
  RootedValue value(cx, AnyRef::fromCompiledCode(valueRef).toJSValue());
  if (!value.isObject()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_OBJECT_REQUIRED, "value");
    return -1;
  }
  RootedObject obj(cx, &value.toObject());
  RootedValue kind(cx);
  if (!GetProperty(cx, obj, obj, cx->names().kind, &kind)) {
    return -1;
  }
  RootedString kindString(cx, ToString(cx, kind));
  if (!kindString) {
    return -1;
  }
  return FindCase(cx, kindString, namesRef);
}

int32_t wasm::ComponentFindEnumCase(Instance* instance, void* valueRef,
                                    void* namesRef) {
  MOZ_ASSERT(SASigComponentFindEnumCase.failureMode ==
             FailureMode::FailOnNegI32);
  JSContext* cx = instance->cx();
  RootedValue value(cx, AnyRef::fromCompiledCode(valueRef).toJSValue());
  RootedString str(cx, ToString(cx, value));
  if (!str) {
    return -1;
  }
  return FindCase(cx, str, namesRef);
}

int32_t wasm::ComponentIsNullish(Instance* instance, void* valueRef) {
  MOZ_ASSERT(SASigComponentIsNullish.failureMode == FailureMode::Infallible);
  return AnyRef::fromCompiledCode(valueRef).toJSValue().isNullOrUndefined();
}

static JSObject* NewScalarArray(JSContext* cx, Scalar::Type type,
                                size_t length) {
  switch (type) {
#  define NEW_ARRAY(ExternalType, NativeType, Name) \
    case Scalar::Name:                              \
      return JS_New##Name##Array(cx, length);
    JS_FOR_EACH_TYPED_ARRAY(NEW_ARRAY)
#  undef NEW_ARRAY
    default:
      MOZ_CRASH("not a typed array type");
  }
}

static JSObject* NewScalarArrayFromArray(JSContext* cx, Scalar::Type type,
                                         HandleObject array) {
  switch (type) {
#  define NEW_ARRAY(ExternalType, NativeType, Name) \
    case Scalar::Name:                              \
      return JS_New##Name##ArrayFromArray(cx, array);
    JS_FOR_EACH_TYPED_ARRAY(NEW_ARRAY)
#  undef NEW_ARRAY
    default:
      MOZ_CRASH("not a typed array type");
  }
}

// Replaces NaNs with the canonical NaN, as the canonical ABI does when it
// loads a float.
template <typename T, typename Bits>
static void CanonicalizeNaNs(T* elems, size_t length, Bits canonicalNaN) {
  for (size_t i = 0; i < length; i++) {
    if (std::isnan(elems[i])) {
      elems[i] = mozilla::BitwiseCast<T>(canonicalNaN);
    }
  }
}

void* wasm::ComponentScalarListToJS(Instance* instance, int32_t ptr,
                                    int32_t length, int32_t kind) {
  MOZ_ASSERT(SASigComponentScalarListToJS.failureMode ==
             FailureMode::FailOnInvalidRef);
  JSContext* cx = instance->cx();
  Scalar::Type type = *ScalarListArrayType(ComponentTypeKind(kind));
  size_t elemSize = Scalar::byteSize(type);
  uint64_t byteLength = uint64_t(uint32_t(length)) * elemSize;
  if (!CheckAlignment(instance, uint32_t(ptr), elemSize)) {
    return AnyRef::invalid().forCompiledCode();
  }
  const uint8_t* src = GetMemoryRange(instance, uint32_t(ptr), byteLength);
  if (!src) {
    return AnyRef::invalid().forCompiledCode();
  }
  // Memory is not moved by GC, so `src` stays valid while allocating.
  JSObject* obj = NewScalarArray(cx, type, uint32_t(length));
  if (!obj) {
    return AnyRef::invalid().forCompiledCode();
  }
  TypedArrayObject& array = obj->as<TypedArrayObject>();
  void* dst = array.dataPointerUnshared();
  memcpy(dst, src, byteLength);
  if (type == Scalar::Float32) {
    CanonicalizeNaNs(static_cast<float*>(dst), uint32_t(length),
                     CanonicalFloat32NaN);
  } else if (type == Scalar::Float64) {
    CanonicalizeNaNs(static_cast<double*>(dst), uint32_t(length),
                     CanonicalFloat64NaN);
  }
  RootedValue value(cx, ObjectValue(array));
  return ExternRefForCompiledCode(cx, value);
}

// Gets a value as a typed array of the given type, or null.
static TypedArrayObject* AsScalarArray(const Value& value, int32_t kind) {
  if (!value.isObject() || !value.toObject().is<TypedArrayObject>()) {
    return nullptr;
  }
  TypedArrayObject& array = value.toObject().as<TypedArrayObject>();
  return array.type() == *ScalarListArrayType(ComponentTypeKind(kind))
             ? &array
             : nullptr;
}

int32_t wasm::ComponentIsScalarArray(Instance* instance, void* valueRef,
                                     int32_t kind) {
  MOZ_ASSERT(SASigComponentIsScalarArray.failureMode ==
             FailureMode::Infallible);
  return !!AsScalarArray(AnyRef::fromCompiledCode(valueRef).toJSValue(), kind);
}

// Gets the length of a typed array, or throws if its buffer is detached, it is
// out of bounds, or its elements wouldn't fit in a 32-bit memory.
static bool GetScalarArrayLength(JSContext* cx, TypedArrayObject* array,
                                 uint32_t* length) {
  mozilla::Maybe<size_t> maybeLength = array->length();
  if (!maybeLength) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TYPED_ARRAY_DETACHED);
    return false;
  }
  if (*maybeLength > INT32_MAX ||
      uint64_t(*maybeLength) * array->bytesPerElement() > UINT32_MAX) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_WASM_COMPONENT_LIST_TOO_LONG);
    return false;
  }
  *length = *maybeLength;
  return true;
}

int32_t wasm::ComponentScalarArrayLength(Instance* instance, void* valueRef,
                                         int32_t kind) {
  MOZ_ASSERT(SASigComponentScalarArrayLength.failureMode ==
             FailureMode::FailOnNegI32);
  uint32_t length;
  if (!GetScalarArrayLength(
          instance->cx(),
          AsScalarArray(AnyRef::fromCompiledCode(valueRef).toJSValue(), kind),
          &length)) {
    return -1;
  }
  return int32_t(length);
}

int32_t wasm::ComponentCopyScalars(Instance* instance, void* valueRef,
                                   int32_t ptr, int32_t length) {
  MOZ_ASSERT(SASigComponentCopyScalars.failureMode ==
             FailureMode::FailOnNegI32);
  JSContext* cx = instance->cx();
  TypedArrayObject& array = AnyRef::fromCompiledCode(valueRef)
                                .toJSValue()
                                .toObject()
                                .as<TypedArrayObject>();

  // realloc can run JS that changes the array after its length was read.
  uint32_t currentLength;
  if (!GetScalarArrayLength(cx, &array, &currentLength)) {
    return -1;
  }
  if (currentLength != uint32_t(length)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_WASM_COMPONENT_BAD_LENGTH);
    return -1;
  }
  size_t elemSize = array.bytesPerElement();
  uint64_t byteLength = uint64_t(uint32_t(length)) * elemSize;
  if (!CheckAlignment(instance, uint32_t(ptr), elemSize)) {
    return -1;
  }
  uint8_t* dst = GetMemoryRange(instance, uint32_t(ptr), byteLength);
  if (!dst) {
    return -1;
  }
  // The array could be a view of the memory itself.
  memmove(dst, array.dataPointerEither().unwrap(), byteLength);
  return 0;
}

void* wasm::ComponentScalarListToArray(Instance* instance, void* valueRef,
                                       int32_t kind) {
  MOZ_ASSERT(SASigComponentScalarListToArray.failureMode ==
             FailureMode::FailOnInvalidRef);
  JSContext* cx = instance->cx();
  RootedValue value(cx, AnyRef::fromCompiledCode(valueRef).toJSValue());
  Rooted<TypedArrayObject*> typedArray(cx, AsScalarArray(value, kind));
  if (!typedArray) {
    return IterableToArrayRef(cx, value, 0);
  }
  uint32_t length;
  if (!GetScalarArrayLength(cx, typedArray, &length)) {
    return AnyRef::invalid().forCompiledCode();
  }
  Rooted<ArrayObject*> array(cx, NewDenseFullyAllocatedArray(cx, length));
  if (!array) {
    return AnyRef::invalid().forCompiledCode();
  }
  RootedValue elem(cx);
  for (uint32_t i = 0; i < length; i++) {
    if (!GetElement(cx, typedArray, typedArray, i, &elem) ||
        !DefineDataElement(cx, array, i, elem)) {
      return AnyRef::invalid().forCompiledCode();
    }
  }
  RootedValue arrayValue(cx, ObjectValue(*array));
  return ExternRefForCompiledCode(cx, arrayValue);
}

void* wasm::ComponentArrayToScalarList(Instance* instance, void* arrayRef,
                                       int32_t kind) {
  MOZ_ASSERT(SASigComponentArrayToScalarList.failureMode ==
             FailureMode::FailOnInvalidRef);
  JSContext* cx = instance->cx();
  RootedObject array(cx, &ArrayFromRef(arrayRef));
  JSObject* typedArray = NewScalarArrayFromArray(
      cx, *ScalarListArrayType(ComponentTypeKind(kind)), array);
  if (!typedArray) {
    return AnyRef::invalid().forCompiledCode();
  }
  RootedValue value(cx, ObjectValue(*typedArray));
  return ExternRefForCompiledCode(cx, value);
}

void* wasm::ComponentToString(Instance* instance, void* valueRef) {
  MOZ_ASSERT(SASigComponentToString.failureMode ==
             FailureMode::FailOnInvalidRef);
  JSContext* cx = instance->cx();
  RootedValue value(cx, AnyRef::fromCompiledCode(valueRef).toJSValue());
  JSString* str = ToString(cx, value);
  if (!str) {
    return AnyRef::invalid().forCompiledCode();
  }
  RootedValue strValue(cx, StringValue(str));
  return ExternRefForCompiledCode(cx, strValue);
}

int32_t wasm::ComponentStringLength(Instance* instance, void* strRef) {
  MOZ_ASSERT(SASigComponentStringLength.failureMode == FailureMode::Infallible);
  return int32_t(
      AnyRef::fromCompiledCode(strRef).toJSValue().toString()->length());
}

static JSLinearString* LinearStringFromRef(JSContext* cx, void* strRef) {
  return AnyRef::fromCompiledCode(strRef).toJSValue().toString()->ensureLinear(
      cx);
}

int32_t wasm::ComponentStoreASCII(Instance* instance, void* strRef, int32_t ptr,
                                  int32_t length) {
  MOZ_ASSERT(SASigComponentStoreASCII.failureMode == FailureMode::FailOnNegI32);
  JSLinearString* str = LinearStringFromRef(instance->cx(), strRef);
  if (!str) {
    return -1;
  }
  uint8_t* dst = GetMemoryRange(instance, uint32_t(ptr), uint32_t(length));
  if (!dst) {
    return -1;
  }

  JS::AutoCheckCannotGC nogc;
  size_t i = 0;
  for (; i < str->length(); i++) {
    char16_t c = str->latin1OrTwoByteChar(i);
    if (c >= 0x80) {
      break;
    }
    dst[i] = uint8_t(c);
  }
  return int32_t(i);
}

int32_t wasm::ComponentStoreUTF8(Instance* instance, void* strRef, int32_t ptr,
                                 int32_t length) {
  MOZ_ASSERT(SASigComponentStoreUTF8.failureMode ==
             FailureMode::FailOnInstanceFlag);
  JSLinearString* str = LinearStringFromRef(instance->cx(), strRef);
  if (!str) {
    return instance->failBuiltin<int32_t>();
  }
  uint8_t* dst = GetMemoryRange(instance, uint32_t(ptr), uint32_t(length));
  if (!dst) {
    return instance->failBuiltin<int32_t>();
  }

  // This replaces lone surrogates with U+FFFD, as for a USVString. It also
  // rewrites the ASCII prefix that ComponentStoreASCII already wrote, which is
  // unobservable.
  size_t written = JS::DeflateStringToUTF8Buffer(
      str, mozilla::Span(reinterpret_cast<char*>(dst), uint32_t(length)));
  MOZ_ASSERT(written == JS::GetDeflatedUTF8StringLength(str));
  return int32_t(written);
}

int32_t wasm::ComponentStoreUTF16(Instance* instance, void* strRef, int32_t ptr,
                                  int32_t length) {
  MOZ_ASSERT(SASigComponentStoreUTF16.failureMode == FailureMode::FailOnNegI32);
  JSLinearString* str = LinearStringFromRef(instance->cx(), strRef);
  if (!str || !CheckAlignment(instance, uint32_t(ptr), 2)) {
    return -1;
  }
  uint8_t* dst =
      GetMemoryRange(instance, uint32_t(ptr), 2 * uint64_t(uint32_t(length)));
  if (!dst) {
    return -1;
  }

  // Replace lone surrogates with U+FFFD, as for a USVString.
  JS::AutoCheckCannotGC nogc;
  size_t n = str->length();
  for (size_t i = 0; i < n; i++) {
    char16_t c = str->latin1OrTwoByteChar(i);
    if (unicode::IsLeadSurrogate(c) && i + 1 < n &&
        unicode::IsTrailSurrogate(str->latin1OrTwoByteChar(i + 1))) {
      mozilla::LittleEndian::writeUint16(dst + 2 * i, c);
      i++;
      c = str->latin1OrTwoByteChar(i);
    } else if (unicode::IsSurrogate(c)) {
      c = unicode::REPLACEMENT_CHARACTER;
    }
    mozilla::LittleEndian::writeUint16(dst + 2 * i, c);
  }
  return 0;
}

int32_t wasm::ComponentStoreLatin1(Instance* instance, void* strRef,
                                   int32_t ptr, int32_t length) {
  MOZ_ASSERT(SASigComponentStoreLatin1.failureMode ==
             FailureMode::FailOnNegI32);
  JSLinearString* str = LinearStringFromRef(instance->cx(), strRef);
  if (!str || !CheckAlignment(instance, uint32_t(ptr), 2)) {
    return -1;
  }
  uint8_t* dst = GetMemoryRange(instance, uint32_t(ptr), uint32_t(length));
  if (!dst) {
    return -1;
  }

  JS::AutoCheckCannotGC nogc;
  size_t i = 0;
  for (; i < str->length(); i++) {
    char16_t c = str->latin1OrTwoByteChar(i);
    if (c > JSString::MAX_LATIN1_CHAR) {
      break;
    }
    dst[i] = uint8_t(c);
  }
  return int32_t(i);
}

int32_t wasm::ComponentStoreLatin1ToUTF16(Instance* instance, void* strRef,
                                          int32_t ptr, int32_t latin1Length) {
  MOZ_ASSERT(SASigComponentStoreLatin1ToUTF16.failureMode ==
             FailureMode::FailOnNegI32);
  JSLinearString* str = LinearStringFromRef(instance->cx(), strRef);
  if (!str || !CheckAlignment(instance, uint32_t(ptr), 2)) {
    return -1;
  }
  size_t length = str->length();
  uint8_t* dst = GetMemoryRange(instance, uint32_t(ptr), 2 * uint64_t(length));
  if (!dst) {
    return -1;
  }

  // Inflate the Latin-1 prefix, which realloc moved to `ptr`, in place.
  for (int32_t i = latin1Length - 1; i >= 0; i--) {
    dst[2 * i] = dst[i];
    dst[2 * i + 1] = 0;
  }

  // Then store the rest as UTF-16, replacing lone surrogates with U+FFFD as
  // for a USVString.
  JS::AutoCheckCannotGC nogc;
  for (size_t i = latin1Length; i < length; i++) {
    char16_t c = str->latin1OrTwoByteChar(i);
    if (unicode::IsLeadSurrogate(c) && i + 1 < length &&
        unicode::IsTrailSurrogate(str->latin1OrTwoByteChar(i + 1))) {
      mozilla::LittleEndian::writeUint16(dst + 2 * i, c);
      i++;
      c = str->latin1OrTwoByteChar(i);
    } else if (unicode::IsSurrogate(c)) {
      c = unicode::REPLACEMENT_CHARACTER;
    }
    mozilla::LittleEndian::writeUint16(dst + 2 * i, c);
  }
  return 0;
}

// The canonical ABI's load_string_from_range, converted to a JS string.
void* wasm::ComponentStringToJS(Instance* instance, int32_t ptr,
                                int32_t taggedLength, int32_t encoding) {
  MOZ_ASSERT(SASigComponentStringToJS.failureMode ==
             FailureMode::FailOnInvalidRef);
  JSContext* cx = instance->cx();

  uint32_t length = uint32_t(taggedLength);
  bool isLatin1 = false;
  uint32_t alignment = 2;
  switch (ComponentStringEncoding(encoding)) {
    case ComponentStringEncoding::UTF8:
      alignment = 1;
      break;
    case ComponentStringEncoding::UTF16:
      break;
    case ComponentStringEncoding::Latin1PlusUTF16: {
      isLatin1 = !(length & UTF16Tag);
      length &= ~UTF16Tag;
      break;
    }
  }
  bool isUTF8 =
      ComponentStringEncoding(encoding) == ComponentStringEncoding::UTF8;
  uint64_t byteLength = isUTF8 || isLatin1 ? length : 2 * uint64_t(length);

  constexpr uint64_t MaxStringByteLength = (1 << 28) - 1;
  if (byteLength > MaxStringByteLength) {
    ReportTrapError(cx, JSMSG_WASM_COMPONENT_STRING_TOO_LONG);
    return AnyRef::invalid().forCompiledCode();
  }
  if (!CheckAlignment(instance, uint32_t(ptr), alignment)) {
    return AnyRef::invalid().forCompiledCode();
  }
  const uint8_t* src = GetMemoryRange(instance, uint32_t(ptr), byteLength);
  if (!src) {
    return AnyRef::invalid().forCompiledCode();
  }

  // Memory is not moved by GC, so it's safe to read while allocating.
  JSLinearString* str;
  if (isUTF8) {
    auto chars = mozilla::Span(reinterpret_cast<const char*>(src), length);
    if (!mozilla::IsUtf8(chars)) {
      ReportTrapError(cx, JSMSG_WASM_COMPONENT_BAD_STRING);
      return AnyRef::invalid().forCompiledCode();
    }
    str = NewStringCopyUTF8N(cx, JS::UTF8Chars(chars.data(), length));
  } else if (isLatin1) {
    str = NewStringCopyN<CanGC>(cx, reinterpret_cast<const Latin1Char*>(src),
                                length);
  } else {
    Vector<char16_t, 32> chars(cx);
    if (!chars.resize(length)) {
      return AnyRef::invalid().forCompiledCode();
    }
    for (uint32_t i = 0; i < length; i++) {
      chars[i] = mozilla::LittleEndian::readUint16(src + 2 * i);
    }
    for (uint32_t i = 0; i < length; i++) {
      if (unicode::IsLeadSurrogate(chars[i]) && i + 1 < length &&
          unicode::IsTrailSurrogate(chars[i + 1])) {
        i++;
      } else if (unicode::IsSurrogate(chars[i])) {
        ReportTrapError(cx, JSMSG_WASM_COMPONENT_BAD_STRING);
        return AnyRef::invalid().forCompiledCode();
      }
    }
    str = NewStringCopyN<CanGC>(cx, chars.begin(), length);
  }
  if (!str) {
    return AnyRef::invalid().forCompiledCode();
  }
  RootedValue value(cx, StringValue(str));
  return ExternRefForCompiledCode(cx, value);
}

// Gets the handle at `index`, trapping unless it has the given resource type.
static ComponentResourceHandle* GetResourceHandle(JSContext* cx,
                                                  ComponentInstance& instance,
                                                  int32_t index,
                                                  int32_t resourceType) {
  ComponentResourceHandle* handle = instance.handles().get(uint32_t(index));
  if (!handle) {
    ReportTrapError(cx, JSMSG_WASM_COMPONENT_BAD_HANDLE);
    return nullptr;
  }
  if (handle->resourceType != uint32_t(resourceType)) {
    ReportTrapError(cx, JSMSG_WASM_COMPONENT_WRONG_RESOURCE_TYPE);
    return nullptr;
  }
  return handle;
}

static bool CheckMayLeave(JSContext* cx, const ComponentInstance& instance) {
  if (!instance.mayLeave()) {
    ReportTrapError(cx, JSMSG_WASM_COMPONENT_MAY_NOT_LEAVE);
    return false;
  }
  return true;
}

int32_t wasm::ComponentResourceNew(Instance* instance, void* ctxRef,
                                   int32_t rep, int32_t resourceType) {
  MOZ_ASSERT(SASigComponentResourceNew.failureMode ==
             FailureMode::FailOnNegI32);
  JSContext* cx = instance->cx();
  ComponentInstance& componentInstance = InstanceFromCtxRef(ctxRef);
  if (!CheckMayLeave(cx, componentInstance)) {
    return -1;
  }
  ComponentResourceHandle handle;
  handle.resourceType = uint32_t(resourceType);
  handle.own = true;
  handle.rep = rep;
  uint32_t index;
  if (!componentInstance.handles().add(std::move(handle), &index)) {
    ReportOutOfMemory(cx);
    return -1;
  }
  if (index == 0) {
    ReportTrapError(cx, JSMSG_WASM_COMPONENT_TABLE_FULL);
    return -1;
  }
  return int32_t(index);
}

int32_t wasm::ComponentResourceRep(Instance* instance, void* ctxRef,
                                   int32_t handle, int32_t resourceType) {
  MOZ_ASSERT(SASigComponentResourceRep.failureMode ==
             FailureMode::FailOnInstanceFlag);
  ComponentInstance& componentInstance = InstanceFromCtxRef(ctxRef);
  ComponentResourceHandle* h = GetResourceHandle(
      instance->cx(), componentInstance, handle, resourceType);
  if (!h) {
    return instance->failBuiltin<int32_t>();
  }
  return int32_t(h->rep);
}

// Calls the disposer of an imported resource type, if any, for a dropped own
// handle. Dropping is infallible, so an exception thrown by the disposer traps.
static bool CallHostResourceDisposer(JSContext* cx,
                                     const ComponentInstance& instance,
                                     int32_t resourceType, HandleValue value) {
  RootedValue disposer(
      cx, ObjectOrNullValue(instance.hostResourceDisposer(resourceType)));
  if (disposer.isNull()) {
    return true;
  }
  RootedValue rval(cx);
  if (Call(cx, disposer, value, &rval)) {
    return true;
  }
  if (cx->isExceptionPending()) {
    cx->clearPendingException();
    ReportTrapError(cx, JSMSG_WASM_COMPONENT_DISPOSE_EXCEPTION);
  }
  return false;
}

int64_t wasm::ComponentResourceDrop(Instance* instance, void* ctxRef,
                                    int32_t handle, int32_t resourceType) {
  MOZ_ASSERT(SASigComponentResourceDrop.failureMode ==
             FailureMode::FailOnInstanceFlag);
  JSContext* cx = instance->cx();
  ComponentInstance& componentInstance = InstanceFromCtxRef(ctxRef);
  if (!CheckMayLeave(cx, componentInstance)) {
    return instance->failBuiltin<int64_t>();
  }
  ComponentResourceHandle* h =
      GetResourceHandle(cx, componentInstance, handle, resourceType);
  if (!h) {
    return instance->failBuiltin<int64_t>();
  }
  if (h->numLends != 0) {
    ReportTrapError(cx, JSMSG_WASM_COMPONENT_HANDLE_LENT);
    return instance->failBuiltin<int64_t>();
  }
  ComponentResourceHandle removed;
  componentInstance.handles().remove(uint32_t(handle), &removed);
  if (!removed.own) {
    MOZ_ASSERT(removed.borrowScope < componentInstance.callScopeDepth());
    componentInstance.callScope(removed.borrowScope).numBorrows--;
    return -1;
  }
  if (componentInstance.hostResourceCtor(resourceType)) {
    RootedValue value(cx, removed.hostValue);
    if (!CallHostResourceDisposer(cx, componentInstance, resourceType, value)) {
      return instance->failBuiltin<int64_t>();
    }
    return -1;
  }
  return int64_t(uint32_t(removed.rep));
}

int32_t wasm::ComponentLowerResource(Instance* instance, void* ctxRef,
                                     void* valueRef, int32_t resourceType,
                                     int32_t borrow) {
  MOZ_ASSERT(SASigComponentLowerResource.failureMode ==
             FailureMode::FailOnNegI32);
  JSContext* cx = instance->cx();
  RootedObject ctx(cx, &AnyRef::fromCompiledCode(ctxRef).toJSObject());
  RootedValue value(cx, AnyRef::fromCompiledCode(valueRef).toJSValue());
  ComponentInstance& componentInstance = InstanceFromCtx(ctx);

  RootedObject ctor(cx, componentInstance.hostResourceCtor(resourceType));
  if (!ctor) {
    JS_ReportErrorASCII(cx, "guest resources are not supported yet");
    return -1;
  }
  bool isInstance;
  if (!InstanceofOperator(cx, ctor, value, &isInstance)) {
    return -1;
  }
  if (!isInstance) {
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                             JSMSG_WASM_COMPONENT_BAD_RESOURCE);
    return -1;
  }

  ComponentResourceHandle handle;
  handle.resourceType = uint32_t(resourceType);
  handle.own = !borrow;
  handle.hostValue = value;
  if (borrow) {
    MOZ_ASSERT(componentInstance.callScopeDepth() > 0);
    handle.borrowScope = componentInstance.callScopeDepth() - 1;
  }
  uint32_t index;
  if (!componentInstance.handles().add(std::move(handle), &index)) {
    ReportOutOfMemory(cx);
    return -1;
  }
  if (index == 0) {
    ReportTrapError(cx, JSMSG_WASM_COMPONENT_TABLE_FULL);
    return -1;
  }
  if (borrow) {
    componentInstance.callScope(componentInstance.callScopeDepth() - 1)
        .numBorrows++;
  }

  // Modifying the handle table enters the guest.
  componentInstance.setEnteredGuest(true);
  return int32_t(index);
}

void* wasm::ComponentLiftResource(Instance* instance, void* ctxRef,
                                  int32_t handle, int32_t resourceType,
                                  int32_t borrow) {
  MOZ_ASSERT(SASigComponentLiftResource.failureMode ==
             FailureMode::FailOnInvalidRef);
  JSContext* cx = instance->cx();
  RootedObject ctx(cx, &AnyRef::fromCompiledCode(ctxRef).toJSObject());
  ComponentInstance& componentInstance = InstanceFromCtx(ctx);
  if (!componentInstance.hostResourceCtor(resourceType)) {
    JS_ReportErrorASCII(cx, "guest resources are not supported yet");
    return AnyRef::invalid().forCompiledCode();
  }

  ComponentResourceHandle* h =
      GetResourceHandle(cx, componentInstance, handle, resourceType);
  if (!h) {
    return AnyRef::invalid().forCompiledCode();
  }
  RootedValue value(cx, h->hostValue);
  if (borrow) {
    MOZ_ASSERT(componentInstance.callScopeDepth() > 0);
    ComponentCallScope& scope =
        componentInstance.callScope(componentInstance.callScopeDepth() - 1);
    if (!scope.lentHandles.append(uint32_t(handle))) {
      ReportOutOfMemory(cx);
      return AnyRef::invalid().forCompiledCode();
    }
    h->numLends++;
  } else {
    if (h->numLends != 0) {
      ReportTrapError(cx, JSMSG_WASM_COMPONENT_HANDLE_LENT);
      return AnyRef::invalid().forCompiledCode();
    }
    if (!h->own) {
      ReportTrapError(cx, JSMSG_WASM_COMPONENT_NOT_OWNED);
      return AnyRef::invalid().forCompiledCode();
    }
    ComponentResourceHandle removed;
    componentInstance.handles().remove(uint32_t(handle), &removed);
  }
  return ExternRefForCompiledCode(cx, value);
}

int32_t wasm::ComponentPushCallScope(Instance* instance, void* ctxRef) {
  MOZ_ASSERT(SASigComponentPushCallScope.failureMode ==
             FailureMode::FailOnNegI32);
  if (!InstanceFromCtxRef(ctxRef).pushCallScope()) {
    ReportOutOfMemory(instance->cx());
    return -1;
  }
  return 0;
}

void wasm::ComponentPopCallScope(Instance* instance, void* ctxRef) {
  MOZ_ASSERT(SASigComponentPopCallScope.failureMode == FailureMode::Infallible);
  InstanceFromCtxRef(ctxRef).popCallScope();
}

int32_t wasm::ComponentCheckBorrowsDropped(Instance* instance, void* ctxRef) {
  MOZ_ASSERT(SASigComponentCheckBorrowsDropped.failureMode ==
             FailureMode::FailOnNegI32);
  ComponentInstance& componentInstance = InstanceFromCtxRef(ctxRef);
  MOZ_ASSERT(componentInstance.callScopeDepth() > 0);
  if (componentInstance.callScope(componentInstance.callScopeDepth() - 1)
          .numBorrows != 0) {
    ReportTrapError(instance->cx(), JSMSG_WASM_COMPONENT_BORROWS_NOT_DROPPED);
    return -1;
  }
  return 0;
}

void wasm::ComponentSetMayLeave(Instance* instance, void* ctxRef,
                                int32_t mayLeave) {
  MOZ_ASSERT(SASigComponentSetMayLeave.failureMode == FailureMode::Infallible);
  InstanceFromCtxRef(ctxRef).setMayLeave(mayLeave);
}

int32_t wasm::ComponentCheckMayLeave(Instance* instance, void* ctxRef) {
  MOZ_ASSERT(SASigComponentCheckMayLeave.failureMode ==
             FailureMode::FailOnNegI32);
  return CheckMayLeave(instance->cx(), InstanceFromCtxRef(ctxRef)) ? 0 : -1;
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
    case AdapterTrap::BadDiscriminant:
      ReportTrapError(instance->cx(), JSMSG_WASM_COMPONENT_BAD_DISCRIMINANT);
      break;
  }
  return -1;
}

#endif  // ENABLE_WASM_COMPONENTS
