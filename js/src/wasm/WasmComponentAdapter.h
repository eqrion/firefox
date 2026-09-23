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

// Creates the JS function for a component function lifted from `callee`.
[[nodiscard]] bool CreateLiftedFunction(
    JSContext* cx, Handle<WasmComponentInstanceObject*> instanceObj,
    const ComponentFuncType& funcType, HandleFunction callee,
    Handle<JSAtom*> name, MutableHandleFunction result);

// Creates the core function for a component function lowered from `host`, a
// function read from the imports object. `coreFuncType` is the flattened type
// of the component function.
[[nodiscard]] bool CreateLoweredFunction(
    JSContext* cx, Handle<WasmComponentInstanceObject*> instanceObj,
    const ComponentFuncType& funcType, const TypeDef& coreFuncType,
    HandleObject host, MutableHandleFunction result);

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

// Traps for the given AdapterTrap reason. Always fails.
int32_t CmTrap(Instance* instance, int32_t reason);

}  // namespace wasm
}  // namespace js

#endif  // ENABLE_WASM_COMPONENTS

#endif  // wasm_component_adapter_h
