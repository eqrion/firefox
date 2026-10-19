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
// A lifted function is a native JS function that calls its adapter. The
// adapter takes one externref for each parameter, and:
//
//   1. Converts each parameter with ToComponentValue into flat locals.
//   2. Calls the lifted core function, trapping if it throws.
//   3. Converts the result with ToJSValue, from its flat value.
//
// The native function locks down the component instance if the call traps.
//
// A lowered function is its adapter, which has the flattened core function
// type, and:
//
//   1. Converts each parameter with ToJSValue, from its flat values.
//   2. Calls the host function with one externref for each parameter.
//   3. Converts the result with ToComponentValue, into its flat value.
//
// An exception from the host function or from converting its result traps.
//
// Adapters are generated in WasmComponentAdapter.cpp. AdapterBuilder builds
// the adapter module along with its imports, and emits wasm instructions.
// LiftLowerBuilder adds the conversions of each component type.
//
// Adapter code calls the builtin module functions in WasmComponentBuiltins.h
// for anything that involves JS values. Builtins that need per-instance state
// take the WasmComponentInstanceObject, which every adapter module imports as
// an immutable externref global.

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

}  // namespace wasm
}  // namespace js

#endif  // ENABLE_WASM_COMPONENTS

#endif  // wasm_component_adapter_h
