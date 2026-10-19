/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef wasm_component_builtins_h
#define wasm_component_builtins_h

#ifdef ENABLE_WASM_COMPONENTS

#  include "mozilla/Maybe.h"

#  include "js/ScalarType.h"
#  include "wasm/WasmComponent.h"

// The builtin module functions that component adapters call, for anything that
// involves JS values. See the [SMDOC] in WasmComponentAdapter.h.

namespace js {
namespace wasm {

class Instance;

// Reasons for ComponentTrap.
enum class AdapterTrap : int32_t {
  UncaughtException,
  ImportException,
};

// The tag on the length of a latin1+utf16 string that is stored as UTF-16.
static constexpr uint32_t UTF16Tag = 1u << 31;

// The class of the typed array that a list of scalars of type `kind` is
// represented by, if it is represented by one.
mozilla::Maybe<Scalar::Type> ScalarListArrayType(ComponentTypeKind kind);

// ToComponentValue for an integer of type `kind`, a ComponentTypeKind, as its
// sign- or zero-extended bits.
int64_t ComponentToInteger(Instance* instance, void* valueRef, int32_t kind);

// ToComponentValue for f64 (and f32 before rounding).
double ComponentToFloat(Instance* instance, void* valueRef);

// ToComponentValue for bool and char.
int32_t ComponentToBool(Instance* instance, void* valueRef);
int32_t ComponentToChar(Instance* instance, void* valueRef);

// ToJSValue for scalars. `kind` is the ComponentTypeKind of an integer.
void* ComponentBoolToJS(Instance* instance, int32_t value);
void* ComponentIntegerToJS(Instance* instance, int32_t value, int32_t kind);
void* ComponentInt64ToJS(Instance* instance, int64_t value, int32_t kind);
void* ComponentFloatToJS(Instance* instance, double value);
void* ComponentCharToJS(Instance* instance, int32_t value);

// Records that the current call has entered the guest.
void ComponentEnterGuest(Instance* instance, void* ctxRef);

// Traps unless `count` elements of `elemSize` bytes at `ptr` are in memory,
// and ptr is aligned.
int32_t ComponentCheckRange(Instance* instance, int32_t ptr, int32_t count,
                            int32_t elemSize, int32_t alignment);

// The first part of ToComponentValueList: iterates a JS value to a new dense
// array, whose elements are then converted. Throws if the list would be larger
// than 32-bit memory, given its elements' size.
void* ComponentIterableToArray(Instance* instance, void* valueRef,
                               int32_t elemSize);
int32_t ComponentArrayLength(Instance* instance, void* arrayRef);
// Throws unless the array has exactly `length` elements.
int32_t ComponentCheckArrayLength(Instance* instance, void* arrayRef,
                                  int32_t length);
void* ComponentArrayGet(Instance* instance, void* arrayRef, int32_t index);

// Creates the JS array for a list or tuple, whose elements are then set.
void* ComponentNewArray(Instance* instance, int32_t length);
int32_t ComponentArraySet(Instance* instance, void* arrayRef, int32_t index,
                          void* valueRef);

// ToComponentValueRecord: ComponentCheckObject throws unless the value is an
// object, and ComponentGetField gets a field. `nameRef` is an atom.
int32_t ComponentCheckObject(Instance* instance, void* valueRef);
void* ComponentGetField(Instance* instance, void* objectRef, void* nameRef);

// ToJSValueRecord: creates a null-prototype object, and defines its fields.
void* ComponentNewRecord(Instance* instance);
int32_t ComponentDefineField(Instance* instance, void* objectRef, void* nameRef,
                             void* valueRef);

// ToJSValue for a list of scalars of type `kind`, a ComponentTypeKind, as a
// typed array.
void* ComponentScalarListToJS(Instance* instance, int32_t ptr, int32_t length,
                              int32_t kind);

// ToComponentValueScalarList for a typed array of the list's scalar type
// `kind`: ComponentIsScalarArray tests for one, ComponentScalarArrayLength gets
// its length, and ComponentCopyScalars copies it to memory.
int32_t ComponentIsScalarArray(Instance* instance, void* valueRef,
                               int32_t kind);
int32_t ComponentScalarArrayLength(Instance* instance, void* valueRef,
                                   int32_t kind);
int32_t ComponentCopyScalars(Instance* instance, void* valueRef, int32_t ptr,
                             int32_t length);

// The steps of storing a JS string with the canonical ABI's store_string.
// ComponentToString converts a JS value to a string, and the rest take that
// string.
void* ComponentToString(Instance* instance, void* valueRef);
int32_t ComponentStringLength(Instance* instance, void* strRef);
// Stores the string's code units at `ptr` until the first that is not ASCII,
// and returns how many were stored.
int32_t ComponentStoreASCII(Instance* instance, void* strRef, int32_t ptr,
                            int32_t length);
// Stores the string as UTF-8 in the `length` bytes at `ptr`, and returns how
// many were stored.
int32_t ComponentStoreUTF8(Instance* instance, void* strRef, int32_t ptr,
                           int32_t length);
// Stores the string's `length` code units as UTF-16 at `ptr`.
int32_t ComponentStoreUTF16(Instance* instance, void* strRef, int32_t ptr,
                            int32_t length);
// Stores the string's code units at `ptr` as Latin-1 until the first that is
// not Latin-1, and returns how many were stored.
int32_t ComponentStoreLatin1(Instance* instance, void* strRef, int32_t ptr,
                             int32_t length);
// Inflates the `latin1Length` bytes of Latin-1 at `ptr` to UTF-16 in place,
// and stores the rest of the string after them as UTF-16.
int32_t ComponentStoreLatin1ToUTF16(Instance* instance, void* strRef,
                                    int32_t ptr, int32_t latin1Length);

// The canonical ABI's load_string, converted to a JS string. `encoding` is a
// ComponentStringEncoding.
void* ComponentStringToJS(Instance* instance, int32_t ptr, int32_t taggedLength,
                          int32_t encoding);

// Traps for the given AdapterTrap reason. Always fails.
int32_t ComponentTrap(Instance* instance, int32_t reason);

}  // namespace wasm
}  // namespace js

#endif  // ENABLE_WASM_COMPONENTS

#endif  // wasm_component_builtins_h
