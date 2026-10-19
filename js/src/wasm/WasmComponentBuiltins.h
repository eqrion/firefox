/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef wasm_component_builtins_h
#define wasm_component_builtins_h

#ifdef ENABLE_WASM_COMPONENTS

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

// Traps for the given AdapterTrap reason. Always fails.
int32_t ComponentTrap(Instance* instance, int32_t reason);

}  // namespace wasm
}  // namespace js

#endif  // ENABLE_WASM_COMPONENTS

#endif  // wasm_component_builtins_h
