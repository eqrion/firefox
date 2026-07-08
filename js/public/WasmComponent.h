/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_WasmComponent_h
#define js_WasmComponent_h

#ifdef ENABLE_WASM_COMPONENTS

#  include "mozilla/RefPtr.h"  // RefPtr

#  include "jstypes.h"  // JS_PUBLIC_API

#  include "js/RefCounted.h"  // AtomicRefCounted
#  include "js/TypeDecls.h"   // HandleObject

namespace js {
class WasmMemoryObject;

namespace wasm {

// Every kind of type that can be defined in the component model. Not all
// types are valid in all contexts. Kept in sync with js/src/wasm/
// WasmComponent.h.
enum class ComponentTypeKind : uint8_t {
  Invalid = 0,

  Bool = 0x7f,
  S8 = 0x7e,
  U8 = 0x7d,
  S16 = 0x7c,
  U16 = 0x7b,
  S32 = 0x7a,
  U32 = 0x79,
  S64 = 0x78,
  U64 = 0x77,
  F32 = 0x76,
  F64 = 0x75,
  Char = 0x74,
  String = 0x73,

  Record = 0x72,
  Variant = 0x71,
  List = 0x70,
  Tuple = 0x6f,
  Flags = 0x6e,
  Enum = 0x6d,
  Option = 0x6b,
  Result = 0x6a,
  Own = 0x69,
  Borrow = 0x68,

  Func = 0x40,  // async func types are not a separate kind
  Component = 0x41,
  Instance = 0x42,
  Resource = 0x3f,  // resource types with callbacks are not a separate kind

  Eq = 0x20,
  SubResource = 0x21,

  // "First" and "last" refer to the actual byte value.
  FirstPrimitive = String,
  LastPrimitive = Bool,
};

// The opaque payload backing a composite ComponentType (js/src/wasm/
// WasmComponent.h). Forward-declared only; never dereferenced by an embedder.
class ComponentTypeDef;

// The own/borrow handle table backing a canon lift/lower call's resource
// params/results (js/src/wasm/WasmComponent.h). Forward-declared only; never
// dereferenced by an embedder.
class ComponentHandleTable;

// A live wasm module instance (js/src/wasm/WasmInstance.h). Forward-declared
// only; never dereferenced by an embedder.
class Instance;

// The component instance's `string-encoding` canonopt: how a component
// `string`/`char` source's bytes are encoded in linear memory.
enum class ComponentStringEncoding : uint8_t { Utf8, Utf16, Latin1OrUtf16 };

}  // namespace wasm
}  // namespace js

namespace JS {

/**
 * TODO(wasm-cm): Leave a descriptive comment here :)
 * For comparison, see WasmModule.h.
 */

struct WasmComponent : js::AtomicRefCounted<WasmComponent> {
  virtual ~WasmComponent() = default;
  virtual JSObject* createObject(JSContext* cx) const = 0;
};

extern JS_PUBLIC_API bool IsWasmComponentObject(HandleObject obj);

extern JS_PUBLIC_API RefPtr<WasmComponent> GetWasmComponent(HandleObject obj);

}  // namespace JS

#endif /* ENABLE_WASM_COMPONENTS */

#endif /* js_WasmComponent_h */
