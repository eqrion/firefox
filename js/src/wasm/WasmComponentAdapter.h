/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef wasm_component_adapter_h
#define wasm_component_adapter_h

#ifdef ENABLE_WASM_COMPONENTS

#  include "wasm/WasmComponent.h"

namespace js {
namespace wasm {

class Instance;

// [SMDOC] Component adapters
//
// Values cross the boundary between JS and a component through adapter
// functions, one for each lifted function that is exported to JS and each
// function import that is lowered into a core instance. An adapter is the only
// function of a small wasm module generated for it. The adapter fuses the
// canonical ABI with the JS-API's ToJSValue and ToComponentValue conversions,
// so that values are converted directly between JS and their core wasm
// representation.
//
// Adapter code calls the builtin module functions below (Cm*) for anything
// that involves JS values. Builtins that need per-instance state take the
// WasmComponentInstanceObject, which every adapter module imports as an
// immutable externref global.

// The runtime values of a lifted or lowered function's canonical options.
struct MOZ_STACK_CLASS CanonOptValues {
  ComponentStringEncoding stringEncoding = ComponentStringEncoding::UTF8;
  Rooted<WasmMemoryObject*> memory;
  RootedFunction realloc;
  RootedFunction postReturn;

  explicit CanonOptValues(JSContext* cx)
      : memory(cx), realloc(cx), postReturn(cx) {}
};

// Creates the JS function for a component function lifted from `callee`.
[[nodiscard]] bool CreateLiftedFunction(
    JSContext* cx, Handle<WasmComponentInstanceObject*> instanceObj,
    const ComponentFuncType& funcType, HandleFunction callee,
    const CanonOptValues& opts, Handle<JSAtom*> name,
    MutableHandleFunction result);

// Creates the core function for a component function lowered from `host`, a
// function read from the imports object. `coreFuncType` is the flattened type
// of the component function.
[[nodiscard]] bool CreateLoweredFunction(
    JSContext* cx, Handle<WasmComponentInstanceObject*> instanceObj,
    const ComponentFuncType& funcType, const TypeDef& coreFuncType,
    HandleObject host, const CanonOptValues& opts,
    MutableHandleFunction result);

// Creates the core function for a canon resource.new, resource.rep, or
// resource.drop of the resource type at index `resourceType` of
// Component::resourceTypes(). `dtor` is the destructor that resource.drop
// calls, if any.
[[nodiscard]] bool CreateResourceBuiltinFunction(
    JSContext* cx, Handle<WasmComponentInstanceObject*> instanceObj,
    ComponentResourceBuiltin::Kind kind, uint32_t resourceType,
    const TypeDef& coreFuncType, HandleFunction dtor,
    MutableHandleFunction result);

// ToComponentValue for an integer of type `kind`, a ComponentTypeKind. On
// success, returns 0 and puts the integer's bits in the instance's scratch.
int32_t CmToInteger(Instance* instance, void* ctxRef, void* valueRef,
                    int32_t kind);

// ToComponentValue for f64 (and f32 before rounding). On success, returns 0
// and puts the double's bits in the instance's scratch.
int32_t CmToFloat(Instance* instance, void* ctxRef, void* valueRef);

// Read the instance's scratch.
int32_t CmScratchI32(Instance* instance, void* ctxRef);
int64_t CmScratchI64(Instance* instance, void* ctxRef);
double CmScratchF64(Instance* instance, void* ctxRef);

// ToComponentValue for bool and char.
int32_t CmToBool(Instance* instance, void* valueRef);
int32_t CmToChar(Instance* instance, void* valueRef);

// ToJSValue for scalars. `kind` is the ComponentTypeKind of an integer.
void* CmBoolToJS(Instance* instance, int32_t value);
void* CmIntegerToJS(Instance* instance, int32_t value, int32_t kind);
void* CmInt64ToJS(Instance* instance, int64_t value, int32_t kind);
void* CmFloatToJS(Instance* instance, double value);
void* CmCharToJS(Instance* instance, int32_t value);

// Records that the current call has entered the guest.
void CmEnterGuest(Instance* instance, void* ctxRef);

// Records that the lifted function being called returned a `result` error,
// with its payload as the data of the ComponentError to throw, if `hasData`.
void CmSetComponentError(Instance* instance, void* ctxRef, void* dataRef,
                         int32_t hasData);

// Gets the JS value of a caught exception, unwrapping a JS exception.
void* CmExceptionValue(Instance* instance, void* exnRef);

// Traps unless `count` elements of `elemSize` bytes at `ptr` are in memory,
// and ptr is aligned.
int32_t CmCheckRange(Instance* instance, int32_t ptr, int32_t count,
                     int32_t elemSize, int32_t alignment);

// The first part of ToComponentValueList: iterates a JS value to a new dense
// array, whose elements are then converted. Throws if the list would be larger
// than 32-bit memory, given its elements' size.
void* CmIterableToArray(Instance* instance, void* valueRef, int32_t elemSize);
int32_t CmArrayLength(Instance* instance, void* arrayRef);
// Throws unless the array has exactly `length` elements.
int32_t CmCheckArrayLength(Instance* instance, void* arrayRef, int32_t length);
void* CmArrayGet(Instance* instance, void* arrayRef, int32_t index);

// Creates the JS array for a list or tuple, whose elements are then set.
void* CmNewArray(Instance* instance, int32_t length);
int32_t CmArraySet(Instance* instance, void* arrayRef, int32_t index,
                   void* valueRef);

// ToComponentValueRecord: CmCheckObject throws unless the value is an object,
// and CmGetField gets a field. `nameRef` is an atom.
int32_t CmCheckObject(Instance* instance, void* valueRef);
void* CmGetField(Instance* instance, void* objectRef, void* nameRef);

// ToJSValueRecord: creates a null-prototype object, and defines its fields.
void* CmNewRecord(Instance* instance);
int32_t CmDefineField(Instance* instance, void* objectRef, void* nameRef,
                      void* valueRef);

// ToJSValueMap: creates a Map and sets its entries.
void* CmNewMap(Instance* instance);
int32_t CmMapSet(Instance* instance, void* mapRef, void* keyRef,
                 void* valueRef);

// The first part of ToComponentValueMap: gets the entries of a map as an
// array. An iterable's entries are iterated, and are then converted like a
// tuple of the key and value. Otherwise, if the keys are strings, the entries
// are the object's own enumerable string-keyed properties, and the instance's
// scratch is set to 1. Throws if the entries would be larger than 32-bit
// memory, given the size of an entry.
void* CmMapEntries(Instance* instance, void* ctxRef, void* valueRef,
                   int32_t entrySize, int32_t keyIsString);

// ToJSValueVariant: creates a null-prototype object with the `kind` and, if
// `hasValue`, the `value`.
void* CmNewVariant(Instance* instance, void* kindRef, void* valueRef,
                   int32_t hasValue);

// Gets the index of the case of a variant or enum, from the `kind` property or
// the value itself. `namesRef` is an array of the cases' JS names.
int32_t CmVariantCase(Instance* instance, void* valueRef, void* namesRef);
int32_t CmEnumCase(Instance* instance, void* valueRef, void* namesRef);

// Whether a value is null or undefined, which is an option's `none`.
int32_t CmIsNullish(Instance* instance, void* valueRef);

// ToJSValue for a list of scalars of type `kind`, a ComponentTypeKind, as a
// typed array.
void* CmScalarListToJS(Instance* instance, int32_t ptr, int32_t length,
                       int32_t kind);

// ToComponentValueScalarList for a typed array of the list's scalar type
// `kind`: CmIsScalarArray tests for one, CmScalarArrayLength gets its length,
// and CmCopyScalars copies it to memory.
int32_t CmIsScalarArray(Instance* instance, void* valueRef, int32_t kind);
int32_t CmScalarArrayLength(Instance* instance, void* valueRef, int32_t kind);
int32_t CmCopyScalars(Instance* instance, void* valueRef, int32_t ptr,
                      int32_t length);

// For a list<T, N> of scalars of type `kind`: CmScalarListToArray gets the
// elements of a typed array of type `kind` as an array, and otherwise iterates
// the value like CmIterableToArray. CmArrayToScalarList creates the typed array
// for an array of the elements.
void* CmScalarListToArray(Instance* instance, void* valueRef, int32_t kind);
void* CmArrayToScalarList(Instance* instance, void* arrayRef, int32_t kind);

// The steps of storing a JS string with the canonical ABI's store_string.
// CmToString converts a JS value to a string, and the rest take that string.
void* CmToString(Instance* instance, void* valueRef);
int32_t CmStringLength(Instance* instance, void* strRef);
// Stores the string's code units at `ptr` until the first that is not ASCII,
// and returns how many were stored.
int32_t CmStoreASCII(Instance* instance, void* strRef, int32_t ptr,
                     int32_t length);
// Stores the string as UTF-8 in the `length` bytes at `ptr`, and puts the
// number of bytes stored in the instance's scratch.
int32_t CmStoreUTF8(Instance* instance, void* ctxRef, void* strRef, int32_t ptr,
                    int32_t length);
// Stores the string's `length` code units as UTF-16 at `ptr`.
int32_t CmStoreUTF16(Instance* instance, void* strRef, int32_t ptr,
                     int32_t length);
// Stores the string's code units at `ptr` as Latin-1 until the first that is
// not Latin-1, and returns how many were stored.
int32_t CmStoreLatin1(Instance* instance, void* strRef, int32_t ptr,
                      int32_t length);
// Inflates the `latin1Length` bytes of Latin-1 at `ptr` to UTF-16 in place,
// and stores the rest of the string after them as UTF-16.
int32_t CmStoreLatin1ToUTF16(Instance* instance, void* strRef, int32_t ptr,
                             int32_t latin1Length);

// The canonical ABI's load_string, converted to a JS string. `encoding` is a
// ComponentStringEncoding.
void* CmStringToJS(Instance* instance, int32_t ptr, int32_t taggedLength,
                   int32_t encoding);

// The canonical ABI's resource.new, resource.rep, and resource.drop, for the
// resource type at index `resourceType` of Component::resourceTypes().
// CmResourceRep puts the rep in the instance's scratch. CmResourceDrop returns
// 1 if the resource's destructor, if any, should be called with the rep in the
// instance's scratch, and 0 otherwise. Dropping an own handle of an imported
// resource type calls the Symbol.dispose of its constructor's prototype.
int32_t CmResourceNew(Instance* instance, void* ctxRef, int32_t rep,
                      int32_t resourceType);
int32_t CmResourceRep(Instance* instance, void* ctxRef, int32_t handle,
                      int32_t resourceType);
int32_t CmResourceDrop(Instance* instance, void* ctxRef, int32_t handle,
                       int32_t resourceType);

// ToComponentValue for an own or borrow of the resource type at index
// `resourceType` of Component::resourceTypes(), returning its handle in the
// instance's table.
int32_t CmLowerResource(Instance* instance, void* ctxRef, void* valueRef,
                        int32_t resourceType, int32_t borrow);
// ToJSValue for an own or borrow of the resource type at index `resourceType`,
// from its handle in the instance's table. An own is removed from the table,
// and a borrow is lent to the current call.
void* CmLiftResource(Instance* instance, void* ctxRef, int32_t handle,
                     int32_t resourceType, int32_t borrow);

// Begins and ends a call out of the instance, which borrowed handles are lent
// to.
int32_t CmPushCallScope(Instance* instance, void* ctxRef);
void CmPopCallScope(Instance* instance, void* ctxRef);
// Traps unless the handles lent to the current call into the instance have
// been dropped.
int32_t CmCheckBorrowsDropped(Instance* instance, void* ctxRef);

// Sets the canonical ABI's may_leave, and traps unless it is set.
void CmSetMayLeave(Instance* instance, void* ctxRef, int32_t mayLeave);
int32_t CmCheckMayLeave(Instance* instance, void* ctxRef);

// Traps for the given AdapterTrap reason. Always fails.
int32_t CmTrap(Instance* instance, int32_t reason);

}  // namespace wasm
}  // namespace js

#endif  // ENABLE_WASM_COMPONENTS

#endif  // wasm_component_adapter_h
