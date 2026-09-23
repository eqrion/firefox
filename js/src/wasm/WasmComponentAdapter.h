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

// Converts a JS value to an integer of type `kind`, a ComponentTypeKind. On
// success, returns 0 and puts the integer in the instance's scratch.
int32_t CmToInteger(Instance* instance, void* ctxRef, void* valueRef,
                    int32_t kind);

// Returns the low 32 bits of the instance's scratch.
int32_t CmScratchI32(Instance* instance, void* ctxRef);

// Converts an integer of type `kind`, a ComponentTypeKind, to a JS value.
void* CmIntegerToJS(Instance* instance, int32_t value, int32_t kind);

// Traps for the given AdapterTrap reason. Always fails.
int32_t CmTrap(Instance* instance, int32_t reason);

}  // namespace wasm
}  // namespace js

#endif  // ENABLE_WASM_COMPONENTS

#endif  // wasm_component_adapter_h
