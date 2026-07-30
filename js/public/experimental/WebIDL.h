/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* An experimental, self-contained model of a subset of the WebIDL type and
 * value system, plus the conversions between JS values and WebIDL values.
 *
 * Standalone: does not depend on the WebAssembly component model, though it
 * exists to support that embedding. */

#ifndef js_experimental_WebIDL_h
#define js_experimental_WebIDL_h

#include "mozilla/Assertions.h"  // MOZ_ASSERT
#include "mozilla/Attributes.h"  // MOZ_STACK_CLASS
#include "mozilla/Maybe.h"       // mozilla::Maybe
#include "mozilla/RefPtr.h"      // RefPtr
#include "mozilla/Span.h"        // mozilla::Span
#include "mozilla/UniquePtr.h"   // mozilla::UniquePtr
#include "mozilla/Variant.h"     // mozilla::Variant
#include "mozilla/Vector.h"      // mozilla::Vector

#include <cstdint>
#include <stdint.h>

#include "jstypes.h"  // JS_PUBLIC_API

#include "js/AllocPolicy.h"                 // js::SystemAllocPolicy
#include "js/experimental/JitInfo.h"        // JSJitInfo
#include "js/experimental/WasmScalarVal.h"  // JS::WasmScalarVal
#include "js/RefCounted.h"                  // js::AtomicRefCounted
#include "js/RootingAPI.h"     // JS::Handle, JS::Rooted, JS::GCPolicy
#include "js/TypeDecls.h"      // JSContext, JSString, JSObject
#include "js/Utility.h"        // JS::UniqueChars
#include "js/Value.h"          // JS::Value
#include "js/WasmComponent.h"  // js::WasmMemoryObject, js::wasm::ComponentTypeKind

namespace JS {

class BigInt;

// The byte values are chosen so the category predicates below
// (IsInteger/IsFloat/IsString/IsNumeric/IsPrimitive) reduce to range checks.
enum class WebIDLTypeKind : uint8_t {
  Invalid = 0,

  Any,
  Undefined,
  Boolean,

  // Integer types. Keep contiguous, Byte first and UnsignedLongLong last.
  Byte,
  Octet,
  Short,
  UnsignedShort,
  Long,
  UnsignedLong,
  LongLong,
  UnsignedLongLong,

  // Float types. Keep contiguous, Float first and UnrestrictedDouble last.
  Float,
  UnrestrictedFloat,
  Double,
  UnrestrictedDouble,

  BigInt,

  // String types. Keep contiguous, DOMString first and ByteString last.
  DOMString,
  USVString,
  ByteString,

  Object,
  Symbol,

  // Types with an associated WebIDLTypeDef payload.
  Interface,
  Callback,
  Dictionary,
  Enumeration,
  Sequence,
  Record,
  Nullable,
  Union,
  BufferSource,
  FrozenArray,
  ObservableArray,
  Promise,
  ReadableStream,

  Limit
};

inline bool WebIDLTypeKindIsInteger(WebIDLTypeKind kind) {
  return WebIDLTypeKind::Byte <= kind &&
         kind <= WebIDLTypeKind::UnsignedLongLong;
}
inline bool WebIDLTypeKindIsFloat(WebIDLTypeKind kind) {
  return WebIDLTypeKind::Float <= kind &&
         kind <= WebIDLTypeKind::UnrestrictedDouble;
}
inline bool WebIDLTypeKindIsNumeric(WebIDLTypeKind kind) {
  return WebIDLTypeKindIsInteger(kind) || WebIDLTypeKindIsFloat(kind);
}
inline bool WebIDLTypeKindIsString(WebIDLTypeKind kind) {
  return WebIDLTypeKind::DOMString <= kind &&
         kind <= WebIDLTypeKind::ByteString;
}
inline bool WebIDLTypeKindIsUnrestrictedFloat(WebIDLTypeKind kind) {
  return kind == WebIDLTypeKind::UnrestrictedFloat ||
         kind == WebIDLTypeKind::UnrestrictedDouble;
}
// A "primitive" type does not carry a WebIDLTypeDef payload.
inline bool WebIDLTypeKindIsPrimitive(WebIDLTypeKind kind) {
  return WebIDLTypeKind::Any <= kind && kind <= WebIDLTypeKind::Symbol;
}

// Extended-attribute modifiers that ride along on a type at its use site.
enum WebIDLTypeFlags : uint8_t {
  WebIDLTypeFlag_None = 0,
  WebIDLTypeFlag_Clamp = 1 << 0,
  WebIDLTypeFlag_EnforceRange = 1 << 1,
  WebIDLTypeFlag_LegacyNullToEmptyString = 1 << 2,
  WebIDLTypeFlag_AllowShared = 1 << 3,
  WebIDLTypeFlag_AllowResizable = 1 << 4,
};

class WebIDLTypeDef;
class WebIDLInterface;
class WebIDLType;

using WebIDLTypeVector = mozilla::Vector<WebIDLType, 0, js::SystemAllocPolicy>;

enum class WebIDLBufferSourceKind : uint8_t {
  ArrayBuffer,
  ArrayBufferView,
  Uint8Array,
  Uint8ClampedArray,
};

// Whether a buffer source is a view over (or a buffer of) plain bytes, with a
// statically known element size of 1. Only these kinds can bridge to/from a
// component `list<u8>`: the conversion is a bulk memcpy, which is only sound
// when the element type and width are known to match. A generic
// `ArrayBufferView` is deliberately excluded - its element type isn't known
// until runtime.
inline bool WebIDLBufferSourceKindIsBytes(WebIDLBufferSourceKind kind) {
  return kind == WebIDLBufferSourceKind::ArrayBuffer ||
         kind == WebIDLBufferSourceKind::Uint8Array ||
         kind == WebIDLBufferSourceKind::Uint8ClampedArray;
}

// A WebIDL interface: name, parent link, and the embedder ops for its dual
// native-pointer/wrapped resource representation. All ops are void*-typed
// (Gecko-agnostic): the embedder closes over the concrete native type.
class WebIDLInterface : public js::AtomicRefCounted<WebIDLInterface> {
  JS::UniqueChars name_;
  RefPtr<WebIDLInterface> parent_;
  void (*addRefNative_)(void*) = nullptr;
  void (*releaseNative_)(void*) = nullptr;
  JSObject* (*reflect_)(JSContext*, void*) = nullptr;
  void* (*unwrapNative_)(JSObject*) = nullptr;

 public:
  WebIDLInterface(JS::UniqueChars name, RefPtr<WebIDLInterface> parent,
                  void (*addRefNative)(void*) = nullptr,
                  void (*releaseNative)(void*) = nullptr,
                  JSObject* (*reflect)(JSContext*, void*) = nullptr,
                  void* (*unwrapNative)(JSObject*) = nullptr)
      : name_(std::move(name)),
        parent_(std::move(parent)),
        addRefNative_(addRefNative),
        releaseNative_(releaseNative),
        reflect_(reflect),
        unwrapNative_(unwrapNative) {}

  const char* name() const { return name_.get(); }
  WebIDLInterface* parent() const { return parent_; }

  // For JIT code calling `releaseNative_` directly. Safe to bake without a
  // null check: hasNativeOps() is asserted true whenever a Native-repr
  // resource handle referencing this interface is created.
  static constexpr size_t offsetOfReleaseNative() {
    return offsetof(WebIDLInterface, releaseNative_);
  }

  // Whether this interface is `other` or (transitively) derives from it.
  bool isOrDerivedFrom(const WebIDLInterface* other) const;

  // Fills in this interface's native ops after construction, if not already
  // set; a no-op if they are. This interface's identity may first be reached
  // (and interned) from a context with no concrete C++ type in scope (e.g. a
  // generic parent-chain reference), before a context that does have the
  // type in scope runs and can supply the real ops.
  void setNativeOps(void (*addRefNative)(void*), void (*releaseNative)(void*),
                    JSObject* (*reflect)(JSContext*, void*),
                    void* (*unwrapNative)(JSObject*)) {
    if (hasNativeOps()) {
      return;
    }
    addRefNative_ = addRefNative;
    releaseNative_ = releaseNative;
    reflect_ = reflect;
    unwrapNative_ = unwrapNative;
  }

  // A resource whose interface has no native ops must always be `Wrapped`.
  bool hasNativeOps() const { return addRefNative_ && releaseNative_; }
  void addRefNative(void* p) const {
    MOZ_ASSERT(addRefNative_);
    addRefNative_(p);
  }
  void releaseNative(void* p) const {
    MOZ_ASSERT(releaseNative_);
    releaseNative_(p);
  }
  // Null if this interface has no `reflect` op, meaning the embedder must
  // keep such a resource `Wrapped`.
  JSObject* reflect(JSContext* cx, void* p) const {
    return reflect_ ? reflect_(cx, p) : nullptr;
  }
  bool canUnwrapNative() const { return unwrapNative_ != nullptr; }
  // Null if `obj` isn't (or can't be confirmed as) an instance of this
  // interface's native type.
  void* unwrapNative(JSObject* obj) const {
    MOZ_ASSERT(unwrapNative_);
    return unwrapNative_(obj);
  }
};

// The type of a WebIDL value or declaration site. A small, cheaply-copyable
// handle: primitive kinds carry only `kind_` (plus `flags_`); composite kinds
// additionally reference a refcounted `WebIDLTypeDef` payload.
class WebIDLType {
  WebIDLTypeKind kind_ = WebIDLTypeKind::Invalid;
  uint8_t flags_ = WebIDLTypeFlag_None;
  RefPtr<WebIDLTypeDef> def_;

  WebIDLType(WebIDLTypeKind kind, uint8_t flags, RefPtr<WebIDLTypeDef> def)
      : kind_(kind), flags_(flags), def_(std::move(def)) {}

 public:
  WebIDLType() = default;
  WebIDLType(const WebIDLType&) = default;
  WebIDLType(WebIDLType&&) = default;
  WebIDLType& operator=(const WebIDLType&) = default;
  WebIDLType& operator=(WebIDLType&&) = default;

  bool isValid() const { return kind_ != WebIDLTypeKind::Invalid; }
  WebIDLTypeKind kind() const { return kind_; }
  uint8_t flags() const { return flags_; }

  bool hasClamp() const { return flags_ & WebIDLTypeFlag_Clamp; }
  bool hasEnforceRange() const { return flags_ & WebIDLTypeFlag_EnforceRange; }
  bool hasLegacyNullToEmptyString() const {
    return flags_ & WebIDLTypeFlag_LegacyNullToEmptyString;
  }
  bool hasAllowShared() const { return flags_ & WebIDLTypeFlag_AllowShared; }
  bool hasAllowResizable() const {
    return flags_ & WebIDLTypeFlag_AllowResizable;
  }

  WebIDLTypeDef* def() const { return def_; }

  // Builders for primitive types. `flags` only meaningful for the kinds that
  // support the corresponding modifier.
  static WebIDLType primitive(WebIDLTypeKind kind,
                              uint8_t flags = WebIDLTypeFlag_None) {
    MOZ_ASSERT(WebIDLTypeKindIsPrimitive(kind));
    return WebIDLType(kind, flags, nullptr);
  }

  // Builders for composite types. These allocate a WebIDLTypeDef and return
  // false on OOM.
  static bool sequence(const WebIDLType& element, WebIDLType* out);
  static bool frozenArray(const WebIDLType& element, WebIDLType* out);
  static bool observableArray(const WebIDLType& element, WebIDLType* out);
  static bool nullable(const WebIDLType& inner, WebIDLType* out);
  static bool promise(const WebIDLType& inner, WebIDLType* out);
  static bool record(WebIDLTypeKind keyStringKind, const WebIDLType& value,
                     WebIDLType* out);
  static bool union_(WebIDLTypeVector&& members, WebIDLType* out);
  static bool enumeration(
      mozilla::Vector<JS::UniqueChars, 0, js::SystemAllocPolicy>&& labels,
      WebIDLType* out);
  static bool interface_(RefPtr<WebIDLInterface> iface, WebIDLType* out);
  static bool callback(WebIDLType* out);
  static bool bufferSource(WebIDLBufferSourceKind kind, uint8_t flags,
                           WebIDLType* out);
  static bool readableStream(WebIDLType* out);

  // A dictionary is built incrementally: create it empty then add members.
  static bool dictionary(WebIDLType* out);
  [[nodiscard]] bool addDictionaryMember(JS::UniqueChars name,
                                         const WebIDLType& type, bool required);

  // Accessors, valid for the corresponding composite kind.
  const WebIDLType& innerType() const;   // sequence/nullable/promise/etc. inner
  WebIDLTypeKind recordKeyKind() const;  // record key string kind
  const WebIDLType& recordValueType() const;
  WebIDLInterface* interface() const;
  WebIDLBufferSourceKind bufferSourceKind() const;

  // Structural equality: same kind, flags, and (recursively) the same composite
  // payload. Union and dictionary members are compared in order.
  static bool structurallyEqual(const WebIDLType& a, const WebIDLType& b);
};

static_assert(std::is_copy_constructible_v<WebIDLType>);

struct WebIDLDictionaryMember {
  JS::UniqueChars name;
  WebIDLType type;
  bool required;
};
using WebIDLDictionaryMemberVector =
    mozilla::Vector<WebIDLDictionaryMember, 0, js::SystemAllocPolicy>;

struct WebIDLDictionaryType {
  WebIDLDictionaryMemberVector members;
};

struct WebIDLRecordType {
  WebIDLTypeKind keyStringKind;
  WebIDLType valueType;
};

struct WebIDLEnumerationType {
  mozilla::Vector<JS::UniqueChars, 0, js::SystemAllocPolicy> labels;

  // Returns the index of `label` in the label set, or Nothing.
  mozilla::Maybe<uint32_t> find(const char* label, size_t length) const;
};

struct WebIDLUnionType {
  WebIDLTypeVector members;
};

struct WebIDLBufferSourceType {
  WebIDLBufferSourceKind kind;
};

struct WebIDLCallbackType {};

using WebIDLTypeSchema =
    mozilla::Variant<mozilla::Nothing, WebIDLType, WebIDLDictionaryType,
                     WebIDLEnumerationType, WebIDLRecordType, WebIDLUnionType,
                     RefPtr<WebIDLInterface>, WebIDLBufferSourceType,
                     WebIDLCallbackType>;

// The payload backing a composite WebIDLType.
class WebIDLTypeDef : public js::AtomicRefCounted<WebIDLTypeDef> {
  WebIDLTypeSchema schema_;

 public:
  explicit WebIDLTypeDef(WebIDLTypeSchema&& schema)
      : schema_(std::move(schema)) {}

  const WebIDLTypeSchema& schema() const { return schema_; }
  WebIDLTypeSchema& schema() { return schema_; }
};

// A single WebIDL attribute.
struct WebIDLAttribute {
  JS::UniqueChars name;
  WebIDLType type;
  bool readonly = false;
  bool isStatic = false;
};

class WebIDLValue;
class WebIDLValueData;

// A small POD tag distinguishing an enumeration value (its label index) from a
// plain integer value.
struct WebIDLEnumValue {
  uint32_t index;
};

// A fully-typed WebIDL value. GC-aware: the object-ish variants hold GC
// pointers, and composite variants reference a refcounted WebIDLValueData
// that owns nested WebIDLValues. Must be used with the rooting APIs
// (Rooted<WebIDLValue>) so its GC edges are kept alive. Cheaply copyable:
// composites share their WebIDLValueData by refcount.
class WebIDLValue {
  using Repr =
      mozilla::Variant<mozilla::Nothing, bool, int64_t, uint64_t, double,
                       JS::Heap<JS::BigInt*>, JS::Heap<JSString*>,
                       JS::Heap<JSObject*>, JS::Heap<JS::Value>,
                       WebIDLEnumValue, RefPtr<WebIDLValueData>>;

  WebIDLType type_;
  Repr repr_ = Repr(mozilla::Nothing());

  WebIDLValue(const WebIDLType& type, Repr&& repr)
      : type_(type), repr_(std::move(repr)) {}

 public:
  WebIDLValue() = default;
  WebIDLValue(const WebIDLValue&) = default;
  WebIDLValue(WebIDLValue&&) = default;
  WebIDLValue& operator=(const WebIDLValue&) = default;
  WebIDLValue& operator=(WebIDLValue&&) = default;

  const WebIDLType& type() const { return type_; }
  WebIDLTypeKind kind() const { return type_.kind(); }

  // Scalar builders (infallible).
  static WebIDLValue makeNull(const WebIDLType& type);
  static WebIDLValue makeUndefined(const WebIDLType& type);
  static WebIDLValue makeBool(const WebIDLType& type, bool b);
  static WebIDLValue makeSigned(const WebIDLType& type, int64_t v);
  static WebIDLValue makeUnsigned(const WebIDLType& type, uint64_t v);
  static WebIDLValue makeDouble(const WebIDLType& type, double v);
  static WebIDLValue makeBigInt(const WebIDLType& type, JS::BigInt* v);
  static WebIDLValue makeString(const WebIDLType& type, JSString* v);
  static WebIDLValue makeObject(const WebIDLType& type, JSObject* v);
  // For `any` (and `symbol`): stores the JS value verbatim.
  static WebIDLValue makeAny(const WebIDLType& type, const JS::Value& v);
  static WebIDLValue makeEnum(const WebIDLType& type, uint32_t index);

  // Composite builders. `data` is a freshly-built WebIDLValueData; these can
  // only fail via the caller's own allocation of `data`.
  static WebIDLValue makeComposite(const WebIDLType& type,
                                   RefPtr<WebIDLValueData> data);

  bool isNothing() const { return repr_.is<mozilla::Nothing>(); }
  bool isNull() const {
    return isNothing() && type_.kind() == WebIDLTypeKind::Nullable;
  }

  bool asBool() const { return repr_.as<bool>(); }
  int64_t asSigned() const { return repr_.as<int64_t>(); }
  uint64_t asUnsigned() const { return repr_.as<uint64_t>(); }
  double asDouble() const { return repr_.as<double>(); }
  JS::BigInt* asBigInt() const {
    return repr_.as<JS::Heap<JS::BigInt*>>().get();
  }
  JSString* asString() const { return repr_.as<JS::Heap<JSString*>>().get(); }
  JSObject* asObject() const { return repr_.as<JS::Heap<JSObject*>>().get(); }
  const JS::Value& asAny() const {
    return repr_.as<JS::Heap<JS::Value>>().get();
  }
  uint32_t asEnumIndex() const { return repr_.as<WebIDLEnumValue>().index; }
  WebIDLValueData* data() const { return repr_.as<RefPtr<WebIDLValueData>>(); }

  bool isBigIntRepr() const { return repr_.is<JS::Heap<JS::BigInt*>>(); }
  bool isStringRepr() const { return repr_.is<JS::Heap<JSString*>>(); }
  bool isObjectRepr() const { return repr_.is<JS::Heap<JSObject*>>(); }
  bool isAnyRepr() const { return repr_.is<JS::Heap<JS::Value>>(); }
  bool isCompositeRepr() const { return repr_.is<RefPtr<WebIDLValueData>>(); }

  void trace(JSTracer* trc);
};

static_assert(std::is_copy_constructible_v<WebIDLValue>);

using WebIDLValueVector =
    mozilla::Vector<WebIDLValue, 0, js::SystemAllocPolicy>;

// The payload backing a composite WebIDLValue (sequence/frozen-array/
// observable-array, dictionary, record). Refcounted so WebIDLValue stays
// copyable.
class WebIDLValueData : public js::AtomicRefCounted<WebIDLValueData> {
 public:
  // A sequence, frozen array or observable array value: ordered elements.
  struct SequenceData {
    WebIDLValueVector elements;
  };
  // A dictionary value: one entry per declared member of the dictionary type,
  // in declaration order.
  struct DictionaryData {
    WebIDLValueVector members;
  };
  // A record value: parallel keys/values.
  struct RecordData {
    mozilla::Vector<JS::Heap<JSString*>, 0, js::SystemAllocPolicy> keys;
    WebIDLValueVector values;
  };

 private:
  mozilla::Variant<SequenceData, DictionaryData, RecordData> data_;

 public:
  explicit WebIDLValueData(SequenceData&& d) : data_(std::move(d)) {}
  explicit WebIDLValueData(DictionaryData&& d) : data_(std::move(d)) {}
  explicit WebIDLValueData(RecordData&& d) : data_(std::move(d)) {}

  bool isSequence() const { return data_.is<SequenceData>(); }
  bool isDictionary() const { return data_.is<DictionaryData>(); }
  bool isRecord() const { return data_.is<RecordData>(); }

  const SequenceData& asSequence() const { return data_.as<SequenceData>(); }
  const DictionaryData& asDictionary() const {
    return data_.as<DictionaryData>();
  }
  const RecordData& asRecord() const { return data_.as<RecordData>(); }

  // Incremental construction. Callers should keep the WebIDLValueData reachable
  // from a root (e.g. via a rooted enclosing WebIDLValue) while appending, so
  // that already-appended GC edges survive intervening allocations.
  [[nodiscard]] bool appendSequenceElement(const WebIDLValue& v) {
    return data_.as<SequenceData>().elements.append(v);
  }
  [[nodiscard]] bool appendDictionaryMember(const WebIDLValue& v) {
    return data_.as<DictionaryData>().members.append(v);
  }
  [[nodiscard]] bool appendRecordEntry(JSString* key, const WebIDLValue& v) {
    RecordData& r = data_.as<RecordData>();
    return r.keys.append(key) && r.values.append(v);
  }

  void trace(JSTracer* trc);
};

using RootedWebIDLValue = JS::Rooted<WebIDLValue>;
using HandleWebIDLValue = JS::Handle<WebIDLValue>;
using MutableHandleWebIDLValue = JS::MutableHandle<WebIDLValue>;

// A single parameter of a WebIDL operation.
struct WebIDLParam {
  WebIDLType type;
  bool optional = false;  // WasSpecified() is false when the caller omits it
  bool variadic = false;  // only the last param of an operation may set this
  // Some() iff this is an optional param with a declared default; consulted
  // only when the caller omits the argument. Never set for a variadic param.
  mozilla::Maybe<WebIDLValue> defaultValue;
};

using WebIDLParamVector =
    mozilla::Vector<WebIDLParam, 0, js::SystemAllocPolicy>;

// The signature of a WebIDL operation: its parameter and return types. Split
// out from WebIDLOperation so a signature can be built and compared on its
// own.
struct WebIDLOperationType {
  WebIDLParamVector params;
  mozilla::Maybe<WebIDLType> returnType;
};

using WebIDLOperationTypeVector =
    mozilla::Vector<WebIDLOperationType, 0, js::SystemAllocPolicy>;

}  // namespace JS

// Composed onto a JSJitInfo (like JSTypedMethodJitInfo), for a native method,
// getter, setter, or constructor whose WebIDL operation type(s) can be
// recovered on request via JS::GetWebIDLOperationTypes/
// GetWebIDLConstructorTypes. `buildOperationTypes` fills `out` with one entry
// per overload (getters/setters always produce exactly one).
struct JSWebIDLMethodJitInfo {
  JSJitInfo base;
  bool (*buildOperationTypes)(JSContext* cx,
                              JS::WebIDLOperationTypeVector& out);
};

namespace JS {

// Recover the WebIDL operation type(s) of a native method, getter, or setter,
// through the private embedding interface installed via a
// JSWebIDLMethodJitInfo (above), keyed off `callable`'s JSJitInfo::
// isWebIDLTyped bit. `callable` need not itself be a function; anything
// without a matching jitinfo is not a WebIDL operation, and `out` gets a
// single `(any...) -> any` fallback entry in that case.
extern JS_PUBLIC_API bool GetWebIDLOperationTypes(
    JSContext* cx, JS::HandleObject callable, WebIDLOperationTypeVector& out);

// Same, for an interface object's constructor: `interfaceObject` must be a
// JSFunction whose JSJitInfo has OpType::StaticMethod and isWebIDLTyped set;
// same fallback as GetWebIDLOperationTypes otherwise.
extern JS_PUBLIC_API bool GetWebIDLConstructorTypes(
    JSContext* cx, JS::HandleObject interfaceObject,
    WebIDLOperationTypeVector& out);

// A single WebIDL operation (a method, static operation, or constructor). Not
// aggregated onto an interface: the embedding binds operations individually.
struct WebIDLOperation {
  JS::UniqueChars name;  // empty for a constructor
  WebIDLOperationType type;
  bool isStatic = false;
};

// The overload "type class" of a WebIDL type, used for distinguishability in
// overload resolution and for the within-kind restriction of CoerceWebIDLValue.
enum class WebIDLOverloadClass : uint8_t {
  Any,
  Undefined,
  Boolean,
  Numeric,
  BigInt,
  String,
  Object,
  Symbol,
  Null,
};

extern JS_PUBLIC_API WebIDLOverloadClass
WebIDLOverloadKind(const WebIDLType& type);

// Convert an ECMAScript value to a WebIDL value of the given type, per WebIDL's
// "convert an ECMAScript value to an IDL value" algorithm. May run JS
// (ToNumber/ToString/iteration/etc.). Throws a TypeError (etc.) on failure.
extern JS_PUBLIC_API bool JSToWebIDLValue(JSContext* cx, JS::HandleValue value,
                                          const WebIDLType& type,
                                          MutableHandleWebIDLValue out);

// Convert a WebIDL value to an ECMAScript value, per WebIDL's "convert an IDL
// value to an ECMAScript value" algorithm.
extern JS_PUBLIC_API bool WebIDLValueToJS(JSContext* cx,
                                          HandleWebIDLValue value,
                                          JS::MutableHandleValue out);

// Coerce a WebIDL value to a different WebIDL type, operating entirely over
// IDL values without invoking JS semantics. May throw TypeError (or
// RangeError under [EnforceRange]).
extern JS_PUBLIC_API bool CoerceWebIDLValue(JSContext* cx,
                                            HandleWebIDLValue from,
                                            const WebIDLType& to,
                                            MutableHandleWebIDLValue out);

}  // namespace JS

#ifdef ENABLE_WASM_COMPONENTS

// The machinery below lets a component import call straight into a WebIDL
// operation, bypassing JS values on the fast path. It talks in WebIDL
// concepts (WebIDLContext, WebIDLCallAdapter, WebIDLCoerceStep) so an
// embedder like Gecko's dom/bindings/Codegen.py can implement
// JS::WebIDLNativeCall entries directly, without depending on js/src/wasm's
// internal headers.

namespace JS {

// The lift/lower options this prototype models: the linear memory to
// read/write, and the `realloc` export used to allocate space for
// strings/lists/records/etc. String encoding is always utf8. Async,
// post-return and callback options are not modeled.
//
// Heap-storable: `memory`/`realloc` are held as JS::Heaps, not JS::Handles,
// so an instance can build one once (at instantiation time) and reuse it
// across calls instead of rebuilding it per call. The owner is responsible
// for calling `trace()`. Both are stored as plain JSObject* (downcast at
// each use, `memory_` to WasmMemoryObject) rather than
// Heap<WasmMemoryObject*> - Heap<T> requires T to be on JS::Heap's public
// allowlist, and WasmMemoryObject, an internal wasm:: type, isn't. `memory`
// is taken as Handle<JSObject*> rather than Handle<WasmMemoryObject*> for
// the same reason one level up: WasmMemoryObject is only forward-declared
// here, so the compiler can't prove it's-a JSObject to convert a Handle to
// one across that incomplete-type boundary - callers (which do have the
// real definition) upcast before calling in.
class WebIDLContext {
  JSContext* cx_ = nullptr;
  Heap<JSObject*> memory_;
  uint8_t* memoryBase_;
  // May be a cross-compartment wrapper around the real `realloc` export.
  Heap<JSObject*> realloc_;
  // Non-owning: `realloc_`'s underlying wasm::Instance + export func index,
  // cached whenever `realloc_` unwraps to a wasm exported function, so
  // `callRealloc` can call it directly via `Instance::callExport` instead of
  // going through the generic `JS::Call` path. Null if `realloc_` isn't a
  // (possibly wrapped) wasm export; kept in sync with `realloc_` by
  // `resolveRealloc`.
  js::wasm::Instance* reallocInstance_ = nullptr;
  uint32_t reallocFuncIndex_ = 0;
  js::wasm::ComponentHandleTable* handles_ = nullptr;

  void resolveRealloc(JS::Handle<JSObject*> realloc);

 public:
  WebIDLContext() = default;
  WebIDLContext(JSContext* cx, JS::Handle<JSObject*> memory,
                uint8_t* memoryBase, JS::Handle<JSObject*> realloc,
                js::wasm::ComponentHandleTable* handles = nullptr)
      : cx_(cx),
        memory_(memory),
        memoryBase_(memoryBase),
        realloc_(realloc),
        handles_(handles) {
    resolveRealloc(realloc);
  }

  // For building a context in stages: `handles()`/`cx()` are known as soon
  // as the owning instance exists, but `memory`/`realloc` may only resolve
  // to a live core export once its core instance has been instantiated.
  void init(JSContext* cx, js::wasm::ComponentHandleTable* handles) {
    cx_ = cx;
    handles_ = handles;
  }
  void setMemoryAndRealloc(JS::Handle<JSObject*> memory, uint8_t* memoryBase,
                           JS::Handle<JSObject*> realloc) {
    memory_ = memory;
    memoryBase_ = memoryBase;
    realloc_ = realloc;
    resolveRealloc(realloc);
  }

  JSContext* cx() const { return cx_; }

  // Must be re-fetched after any call that may grow or move the buffer
  // (e.g. `callRealloc`).
  [[nodiscard]] bool memoryBytes(uint8_t** base, size_t* len) const;

  // Per the canonical ABI's `cx.opts.realloc` calling convention:
  // `realloc(origPtr, origSize, align, newSize) -> newPtr`.
  [[nodiscard]] bool callRealloc(uint32_t origPtr, uint32_t origSize,
                                 uint32_t align, uint32_t newSize,
                                 uint32_t* outPtr) const;

  // The own/borrow handle table for this call, or null if no own/borrow-
  // typed param or result is reachable.
  js::wasm::ComponentHandleTable* handles() const { return handles_; }

  void trace(JSTracer* trc);
};

// A `WebIDLCallAdapter` is compiled once per `(component function type,
// WebIDLOperationType)` pair and describes, for each WebIDL parameter, how
// to read the raw canonical-ABI value straight from flat core params +
// linear memory and coerce it to the target WebIDL type. See
// js::wasm::BuildCallAdapter (js/src/wasm/WasmComponent.h).

// The primitive coercion operations a Scalar-kind WebIDLCoerceStep may
// apply. The target width/signedness/range live on the step itself.
enum class WebIDLCoerceOp : uint8_t {
  BoolIdentity,
  IntIdentity,
  IntWidenSigned,
  IntWidenUnsigned,
  IntModular,
  IntClamp,
  IntEnforceRange,
  // A restricted target (`float`/`double`, not `unrestricted *`) doesn't get
  // its own op - it's one of these ops plus WebIDLCoerceStep::restrictFloat.
  FloatFromSigned,
  FloatFromUnsigned,
  F32ToF64,
  F64ToF32,
  FloatIdentity,
  BigIntFromSigned,
  BigIntFromUnsigned,
  // Component `char` (a flat i32 Unicode scalar value) into a string target;
  // takes the Scalar path, not String (no (ptr,len) in memory).
  CharToString,
  // The result-direction reverse of CharToString.
  CharFromString,
};

enum class WebIDLStepKind : uint8_t {
  Scalar,
  String,
  Enumeration,
  Interface,
  BufferSource,
  Any,
  Object,  // JS escape hatches (produce JS::Value / JSObject*)
  Nullable,
  Sequence,
  Dictionary,
  Union,
  // A discriminant-selected optional payload (component `option<T>` source,
  // `T?` target). Distinct from Nullable: the Option source carries its own
  // present/absent bit that must be read first, rather than recursing with
  // the same source under a narrower target.
  Option,
  // A discriminant-selected N-way case (component `variant`/`result<T,E>`/
  // nested `option<option<T>>`), all despecializing to the same WebIDL
  // Dictionary `{kind, value?}` shape.
  Variant,
  // A value-driven doomed coercion, discovered only once a specific branch
  // is reached (e.g. under a Nullable inner or optional dictionary member).
  // An unconditional failure at the top level is hoisted to
  // WebIDLCallAdapter::isTrap instead.
  Trap,
};

enum class WebIDLStringKind : uint8_t { DOMString, USVString, ByteString };

// How a node reads (or, via a step's `destAccess`, writes) its primitive
// data straight from/to the raw canonical ABI inputs (flat core params/
// results + linear memory). The same shape serves both directions - a
// param's `access` and a result's `destAccess` just live in different
// fields.
//
// `offset`/`elemStride` are relative to whatever base is current at this
// point in a walk, never an absolute address; a Sequence element's own
// WebIDLAccessLoc is relative to its runtime-loaded `ptr`, not to the base
// the Sequence itself was read from.
struct WebIDLAccessLoc {
  enum class Mode : uint8_t {
    FlatSlot,  // flattened param/result slot(s) (top level)
    Mem,       // (enclosing base + offset) in linear memory
    Absent,    // Default / NotPassed / MissingRequired
  } mode = Mode::Absent;
  uint32_t flatSlot = 0;    // FlatSlot: first slot (scalar = 1, ptr+len = 2)
  uint32_t offset = 0;      // Mem: byte offset from the enclosing base
  uint32_t elemStride = 0;  // Sequence: element size, to derive child bases
  // Sequence, destAccess only: the element buffer's own alignment (from
  // Alignment(elemType) at build time) - WriteCoercedListHeader needs this
  // to realloc; a read never allocates, so `access.elemAlign` stays unused.
  uint32_t elemAlign = 0;
};

struct WebIDLCoerceStep;

// One member of a Dictionary-kind WebIDLCoerceStep's target, resolved at
// build time against the component source's fields (Dictionary targets are
// only sourced from a component `record`; any other source kind has no
// fields, so every member resolves to Default or MissingRequired).
struct WebIDLDictMemberStep {
  enum class Presence : uint8_t {
    FromSource,
    Default,
    MissingRequired,
  } presence = Presence::MissingRequired;
  uint32_t sourceField = 0;  // FromSource: component record field
  const JS::WebIDLValue* defaultValue = nullptr;  // Default
  mozilla::UniquePtr<WebIDLCoerceStep> step;      // FromSource
};

using WebIDLDictMemberStepVector =
    mozilla::Vector<WebIDLDictMemberStep, 0, js::SystemAllocPolicy>;

// One case of a Variant-kind WebIDLCoerceStep's source, in source
// declaration order. The discriminant read (JS::ReadCoercedVariantCase)
// yields a case index into this vector; `enumRemap` maps it to the target
// "kind" enum label index.
struct WebIDLVariantCaseStep {
  mozilla::UniquePtr<WebIDLCoerceStep> payloadStep;  // null = no payload
};
using WebIDLVariantCaseStepVector =
    mozilla::Vector<WebIDLVariantCaseStep, 0, js::SystemAllocPolicy>;

struct WebIDLCoerceStep {
  WebIDLStepKind kind = WebIDLStepKind::Scalar;
  // Used by the dynamic interpreter to build output + recurse; skipped by a
  // generated static walk, which debug-asserts `kind` instead.
  JS::WebIDLType targetType;
  WebIDLAccessLoc access;
  // Result-direction only: where a JS::WriteCoerced* call writes this
  // step's value. Default Absent is treated as FlatSlot.
  WebIDLAccessLoc destAccess;
  // Dictionary/Option/Variant result direction only: true when this step's
  // own flattened representation exceeds the flat result budget, so it must
  // own a freshly-allocated buffer when reached as the walk's un-addressed
  // root (destAccess.mode != Mem). A nested occurrence ignores this (its
  // parent already stamped destAccess.mode = Mem onto it) and a
  // non-spilling root instead decomposes directly into flat result slots -
  // see WriteCoercedVariantCase/WriteCoercedOptionDiscriminant and the
  // Dictionary case of the result walk for why that's always safe: a
  // non-spilling composite's every member/payload independently flattens to
  // zero slots too.
  bool resultSpills = false;
  // Dictionary result direction only: the byte size/alignment of this
  // step's own buffer, needed only when it's the top-level result (a
  // Dictionary has no self-describing (ptr,len) header to infer it from at
  // runtime, unlike Sequence/String).
  uint32_t resultSize = 0;
  uint32_t resultAlign = 0;
  // Scalar. `srcBits`/`srcSigned` describe how to interpret the raw i32/i64
  // read off `access`.
  WebIDLCoerceOp op = WebIDLCoerceOp::IntIdentity;
  uint8_t srcBits = 0;
  bool srcSigned = false;
  uint8_t targetBits = 0;
  bool targetSigned = false;
  int64_t rangeMin = 0, rangeMax = 0;
  // Scalar, float ops only: after converting via `op`, trap on NaN/Inf.
  bool restrictFloat = false;
  // String
  js::wasm::ComponentStringEncoding srcEncoding =
      js::wasm::ComponentStringEncoding::Utf8;
  WebIDLStringKind targetString = WebIDLStringKind::DOMString;
  // Enumeration: sourceLabelIndex -> targetLabelIndex; kNoEnumMatch means
  // this source label has no counterpart in the target's label set (a
  // per-value trap, since the label set is only known at that value).
  static constexpr uint32_t kNoEnumMatch = UINT32_MAX;
  mozilla::Vector<uint32_t, 0, js::SystemAllocPolicy> enumRemap;
  // Interface (component `own`/`borrow`): own removes the handle (transfers
  // out), borrow only reads it. The source's component type is stored as
  // its raw (kind, typeDef) parts rather than a js::wasm::ComponentType by
  // value, since that type's full definition depends on wasm-internal
  // headers unsuitable for public exposure.
  JS::WebIDLInterface* expectedInterface = nullptr;
  bool resourceOwn = false;
  js::wasm::ComponentTypeKind resourceTypeKind =
      js::wasm::ComponentTypeKind::Invalid;
  RefPtr<js::wasm::ComponentTypeDef> resourceTypeDef;
  // BufferSource
  JS::WebIDLBufferSourceKind bufferKind =
      JS::WebIDLBufferSourceKind::ArrayBuffer;
  // Any/Object only: the escape hatches accept any canonical shape, so
  // there's no specialized target to derive a fused read from - the
  // dynamic walk falls back to a generic lift of this (kind, typeDef)
  // source type, then the existing ToCanonicalWebIDLValue + WebIDLValueToJS
  // path, for just this leaf.
  js::wasm::ComponentTypeKind genericSourceTypeKind =
      js::wasm::ComponentTypeKind::Invalid;
  RefPtr<js::wasm::ComponentTypeDef> genericSourceTypeDef;
  // Children (owning): Nullable's inner, Sequence's element, or a Union arm.
  mozilla::UniquePtr<WebIDLCoerceStep> child;
  // Dictionary target members, in target declaration order.
  WebIDLDictMemberStepVector dictMembers;
  // Variant target cases, in source declaration order.
  WebIDLVariantCaseStepVector variantCases;
};

// One parameter of the compiled call: how to source its value (positionally
// from the component call, from a declared/absent default, or from the
// variadic tail) and how to coerce it.
struct WebIDLParamStep {
  enum class Source : uint8_t {
    Positional,
    Default,
    VariadicSpread,
  } source = Source::Positional;
  uint32_t componentIndex = 0;  // Positional / spread start (build-time)
  // Default: the declared value, or null/undefined for NotPassed. A
  // required-but-missing param folds into WebIDLCallAdapter::isTrap
  // instead.
  const JS::WebIDLValue* defaultValue = nullptr;
  WebIDLCoerceStep step;
};

using WebIDLParamStepVector =
    mozilla::Vector<WebIDLParamStep, 0, js::SystemAllocPolicy>;

struct WebIDLCallAdapter {
  WebIDLParamStepVector paramSteps;  // one per WebIDL param (decl order)
  bool throwing = false;             // component result is `result<T,E>`
  mozilla::Maybe<WebIDLCoerceStep> resultStep;  // WebIDL return -> ok/T
  mozilla::Maybe<WebIDLCoerceStep> errorStep;   // thrown exception -> E
  bool isTrap = false;                          // whole binding traps at entry
};

// The generated-style "static walk" entry point for one overload: reads its
// params directly from the raw canonical-ABI inputs described by `adapter`,
// builds native (non-JS) arguments, calls a specialized implementation, and
// lowers the result into `results` - never materializing a JS::Value.
// Index-aligned with the WebIDLOperationTypeVector
// JS::GetWebIDLOperationTypes/GetWebIDLConstructorTypes would return for the
// same callee. Implementations read/write params via the
// JS::ReadCoerced*/WriteCoerced* reader library rather than inspecting
// `adapter`/`ctx` directly. `resultMemBase` is the linear-memory offset a
// result whose flat form spills past MaxFlatResults must be written at (the
// canonical ABI's caller-allocated out-param); 0 for a result that fits flat.
// Passed as an explicit argument, not carried on `ctx`, since it is per-call.
//
// Every argument is a plain pointer/integer so this signature is directly
// callable from JIT-generated code with no C++ bridge frame: `params` points
// to as many caller-owned WasmScalarVals as `adapter->paramSteps` addresses
// (the callee never needs a count - every access is a statically-computed
// flatSlot baked into `adapter` at build time, not runtime-bounds-checked);
// `results` points to caller-owned storage for the call's single flat result
// slot (MaxFlatResults == 1), left untouched for a result that spills into
// linear memory via `resultMemBase`/`*step.child` instead, or for no result
// at all.
using WebIDLNativeCall = bool (*)(const WebIDLCallAdapter* adapter,
                                  WebIDLContext* ctx,
                                  const WasmScalarVal* params,
                                  uint32_t resultMemBase,
                                  WasmScalarVal* results);

}  // namespace JS

// Composed onto a JSWebIDLMethodJitInfo, adding the native overload-entry
// table for the WebAssembly component model's fused "wasm calls WebIDL"
// native ABI. Index-aligned with the WebIDLOperationTypeVector
// JS::GetWebIDLOperationTypes/GetWebIDLConstructorTypes would return for the
// same callee; an overload with no qualifying native entry has a null slot,
// falling back to the ordinary dynamic-interpreter/JS::Call path.
struct JSWebIDLNativeMethodJitInfo {
  JSWebIDLMethodJitInfo base;
  const JS::WebIDLNativeCall* overloadEntries;
};

namespace JS {

// Reads a Scalar-kind WebIDLCoerceStep's raw value directly into a native
// int32_t, bypassing JS::Value entirely. Debug-asserts `step.kind ==
// Scalar` and a target of <=32 bits. `memBase` is 0 for a top-level param;
// see WebIDLAccessLoc's doc comment for the general case.
extern JS_PUBLIC_API bool ReadCoercedInt32(WebIDLContext& ctx,
                                           const WasmScalarVal* coreParams,
                                           const WebIDLCoerceStep& step,
                                           uint32_t memBase, int32_t* out);

// Same as ReadCoercedInt32, but for a LongLong/UnsignedLongLong target: the
// full-width native result a 32-bit output can't hold.
extern JS_PUBLIC_API bool ReadCoercedInt64(WebIDLContext& ctx,
                                           const WasmScalarVal* coreParams,
                                           const WebIDLCoerceStep& step,
                                           uint32_t memBase, int64_t* out);

// Reads a Scalar-kind WebIDLCoerceStep's raw floating-point value (a
// Float/UnrestrictedFloat/Double/UnrestrictedDouble target) directly into a
// native double, applying the step's op (widen/narrow between f32/f64, an
// int source converted to float, and - for a restricted target - the
// NaN/Inf trap). See ReadCoercedInt32's doc comment for the general
// contract.
extern JS_PUBLIC_API bool ReadCoercedDouble(WebIDLContext& ctx,
                                            const WasmScalarVal* coreParams,
                                            const WebIDLCoerceStep& step,
                                            uint32_t memBase, double* out);

// Reads a Scalar-kind WebIDLCoerceStep's raw boolean value.
extern JS_PUBLIC_API bool ReadCoercedBool(WebIDLContext& ctx,
                                          const WasmScalarVal* coreParams,
                                          const WebIDLCoerceStep& step,
                                          uint32_t memBase, bool* out);

// Reads a Scalar-kind WebIDLCoerceStep's raw value as a component `char`
// codepoint (CoerceOp::CharToString), taking the Scalar path since there's
// no (ptr,len) in memory.
extern JS_PUBLIC_API bool ReadCoercedCodePoint(WebIDLContext& ctx,
                                               const WasmScalarVal* coreParams,
                                               const WebIDLCoerceStep& step,
                                               uint32_t memBase,
                                               uint32_t* outCp);

// Reads a String-kind WebIDLCoerceStep's raw (ptr,len) byte range in linear
// memory, without decoding it.
extern JS_PUBLIC_API bool ReadCoercedStringRange(
    WebIDLContext& ctx, const WasmScalarVal* coreParams,
    const WebIDLCoerceStep& step, uint32_t memBase, uint32_t* outPtr,
    uint32_t* outLen);

// Reads a Sequence-kind WebIDLCoerceStep's raw (elemPtr,len) header. The
// caller loops `i` in [0,len), derives each element's base as
// `elemPtr + i * step.access.elemStride`, and recurses into `*step.child`.
extern JS_PUBLIC_API bool ReadCoercedListHeader(WebIDLContext& ctx,
                                                const WasmScalarVal* coreParams,
                                                const WebIDLCoerceStep& step,
                                                uint32_t memBase,
                                                uint32_t* outElemPtr,
                                                uint32_t* outLen);

// Reads a BufferSource-kind WebIDLCoerceStep's raw (ptr,len) byte range in
// linear memory, without copying it. The step's source is a component
// `list<u8>` (BuildCoerceStep rejects any other element type for a
// buffer-source target), so `len` is both the element count and the byte
// count. The caller copies those bytes into a freshly-allocated typed array
// rather than aliasing them: linear memory can be grown or detached by later
// wasm execution, and the DOM object outlives this call.
extern JS_PUBLIC_API bool ReadCoercedByteListRange(
    WebIDLContext& ctx, const WasmScalarVal* coreParams,
    const WebIDLCoerceStep& step, uint32_t memBase, uint32_t* outPtr,
    uint32_t* outLen);

// Reads an Interface-kind WebIDLCoerceStep's raw own/borrow handle index and
// resolves it against `ctx`'s handle table (own removes the handle, borrow
// only reads it), returning the resource's JSObject*. Traps if there is no
// handle table, or the handle is missing, of the wrong resource type, or
// (for `own`) not actually owned or still lent out.
extern JS_PUBLIC_API bool ReadCoercedInterface(WebIDLContext& ctx,
                                               const WasmScalarVal* coreParams,
                                               const WebIDLCoerceStep& step,
                                               uint32_t memBase,
                                               JSObject** outObject);

// Same as ReadCoercedInterface, but for the native fast path: resolves the
// handle exactly the same way, then returns the resource's native pointer
// directly if its representation is already Native, or unwraps its Wrapped
// object via `step.expectedInterface`'s unwrapNative op otherwise (failing,
// per the same trap, if that interface has no way to unwrap, or the object
// isn't an instance of it). For an `own` handle, the handle table's own
// strong ref on the returned pointer is released as part of resolving the
// handle - the caller must take its own ref (e.g. a RefPtr) before this
// call's `WebIDLContext` interaction ends, to keep the pointer alive beyond
// that point.
extern JS_PUBLIC_API bool ReadCoercedInterfaceNative(
    WebIDLContext& ctx, const WasmScalarVal* coreParams,
    const WebIDLCoerceStep& step, uint32_t memBase, void** outNative);

// Reads an Enumeration-kind WebIDLCoerceStep's raw source label index and
// applies the step's source-to-target label remap, returning the target
// label's index. Traps if the source index is out of range, or has no
// counterpart in the target's label set.
extern JS_PUBLIC_API bool ReadCoercedEnumIndex(WebIDLContext& ctx,
                                               const WasmScalarVal* coreParams,
                                               const WebIDLCoerceStep& step,
                                               uint32_t memBase,
                                               uint32_t* outTargetIndex);

// Reads an Option-kind WebIDLCoerceStep's present/absent discriminant. If
// `*outHasValue` is true, the caller recurses into `*step.child` at the same
// `memBase` this discriminant was read at.
extern JS_PUBLIC_API bool ReadCoercedOptionDiscriminant(
    WebIDLContext& ctx, const WasmScalarVal* coreParams,
    const WebIDLCoerceStep& step, uint32_t memBase, bool* outHasValue);

// Reads a Variant-kind WebIDLCoerceStep's case discriminant and applies the
// step's source-case-index -> target-"kind"-label remap. If
// `step.variantCases[*outSourceCaseIndex].payloadStep` is non-null, the
// caller recurses into it (at the same `memBase`) for the "value" member.
// Traps if the source case index is out of range, or (once selected) has no
// counterpart in the target's "kind" label set.
extern JS_PUBLIC_API bool ReadCoercedVariantCase(
    WebIDLContext& ctx, const WasmScalarVal* coreParams,
    const WebIDLCoerceStep& step, uint32_t memBase,
    uint32_t* outSourceCaseIndex, uint32_t* outTargetKindIndex);

// The JS::WriteCoerced* family: the result-direction counterpart of
// JS::ReadCoerced* above. Applies the step's coercion to `value`, then
// writes the raw result per `step.destAccess`: FlatSlot (the default) writes
// `*result`; Mem writes at `memBase + step.destAccess.offset`. `result` is
// the call's single flat result slot - the canonical ABI's MaxFlatResults
// budget guarantees a CoerceStep tree rooted at a FlatSlot step writes it at
// most once - so unlike JS::ReadCoerced*'s `coreParams`, there is no index to
// thread through.
extern JS_PUBLIC_API bool WriteCoercedInt32(WebIDLContext& ctx,
                                            const WebIDLCoerceStep& step,
                                            uint32_t memBase, int32_t value,
                                            WasmScalarVal* result);

// Same as WriteCoercedInt32, but for a LongLong/UnsignedLongLong-declared
// return value: the full-width native input a 32-bit input can't hold.
extern JS_PUBLIC_API bool WriteCoercedInt64(WebIDLContext& ctx,
                                            const WebIDLCoerceStep& step,
                                            uint32_t memBase, int64_t value,
                                            WasmScalarVal* result);

// Writes a Scalar-kind step's floating-point result, applying the step's op
// (narrow/widen between f32/f64, or narrow to an integer component target).
extern JS_PUBLIC_API bool WriteCoercedDouble(WebIDLContext& ctx,
                                             const WebIDLCoerceStep& step,
                                             uint32_t memBase, double value,
                                             WasmScalarVal* result);

// Writes a Scalar-kind step's boolean result.
extern JS_PUBLIC_API bool WriteCoercedBool(WebIDLContext& ctx,
                                           const WebIDLCoerceStep& step,
                                           uint32_t memBase, bool value,
                                           WasmScalarVal* result);

// Writes a String-kind step's result: allocates space for `utf8` via
// `ctx.callRealloc` and copies it in, then writes a (ptr,len) header per
// `step.destAccess`.
extern JS_PUBLIC_API bool WriteCoercedString(WebIDLContext& ctx,
                                             const WebIDLCoerceStep& step,
                                             uint32_t memBase, const char* utf8,
                                             size_t len, WasmScalarVal* result);

// Same as WriteCoercedString, but transcodes `utf16` straight into the
// destination allocation instead of requiring the caller to first transcode
// to a separate UTF-8 buffer. Implements the canonical ABI's
// store_utf16_to_utf8: an optimistic 1-byte-per-unit allocation, falling back
// to a worst-case-sized allocation (and a shrinking realloc) only if `utf16`
// turns out not to be all-ASCII.
extern JS_PUBLIC_API bool WriteCoercedStringUTF16(
    WebIDLContext& ctx, const WebIDLCoerceStep& step, uint32_t memBase,
    mozilla::Span<const char16_t> utf16, WasmScalarVal* result);

// Writes a Scalar-kind step's component `char` result (CoerceOp::
// CharFromString, the result-direction reverse of CharToString).
extern JS_PUBLIC_API bool WriteCoercedCodePoint(WebIDLContext& ctx,
                                                const WebIDLCoerceStep& step,
                                                uint32_t memBase, uint32_t cp,
                                                WasmScalarVal* result);

// Writes an Interface-kind step's result: registers `object` as a fresh own
// handle (or re-borrows it, for a `borrow` result) in `ctx`'s handle table,
// then writes the resulting index per `step.destAccess`. Traps if there is
// no handle table.
extern JS_PUBLIC_API bool WriteCoercedInterface(WebIDLContext& ctx,
                                                const WebIDLCoerceStep& step,
                                                uint32_t memBase,
                                                JSObject* object,
                                                WasmScalarVal* result);

// Same as WriteCoercedInterface, but for the native fast path: registers
// `native` as a fresh own handle (or re-borrows it, for a borrow result)
// directly, with no wrapper - one strong ref is taken on `native` via
// `step.expectedInterface`'s addRefNative, for as long as it stays in the
// handle table. `step.expectedInterface` must have native ops
// (WebIDLInterface::hasNativeOps()).
extern JS_PUBLIC_API bool WriteCoercedInterfaceNative(
    WebIDLContext& ctx, const WebIDLCoerceStep& step, uint32_t memBase,
    void* native, WasmScalarVal* result);

// Writes a Sequence-kind step's (elemPtr,len) header: reallocs
// `count * step.destAccess.elemStride` bytes for the element buffer, writes
// the header per `step.destAccess`, and returns the element buffer's base in
// `*outElemPtr`. The caller then writes each element via the matching
// JS::WriteCoerced* on `*step.child`.
extern JS_PUBLIC_API bool WriteCoercedListHeader(
    WebIDLContext& ctx, const WebIDLCoerceStep& step, uint32_t memBase,
    uint32_t count, WasmScalarVal* result, uint32_t* outElemPtr);

// Writes a whole component `list<u8>` result from a byte-typed buffer source
// in one bulk copy: allocates the element buffer via WriteCoercedListHeader,
// then memcpys `bufferSource`'s bytes into it. `step` is a Sequence whose
// element is a single byte (asserted) - which is what BuildResultStep produces
// for a `list<u8>` component result. The buffer-source-ness lives on the
// *WebIDL* side, as the declared return type of the operation that produced
// `bufferSource`.
//
// Takes the object rather than a Span deliberately: allocating the element
// buffer runs the instance's `realloc`, which can GC, move linear memory, and
// even detach or resize `bufferSource` itself. A caller cannot hold a data
// pointer across that, so this owns the whole window - reading the length,
// allocating, then re-acquiring the data under a fresh no-GC region and
// re-checking the length before copying.
//
// `bufferSource` may be a cross-compartment wrapper. Traps if it isn't an
// ArrayBuffer or ArrayBufferView, or if it is detached or changes length
// across the allocation.
extern JS_PUBLIC_API bool WriteCoercedByteListFromBufferSource(
    WebIDLContext& ctx, const WebIDLCoerceStep& step, uint32_t memBase,
    JSObject* bufferSource, WasmScalarVal* result);

// Writes an Option-kind step's discriminant, allocating its own buffer when
// top-level or resolving into the enclosing composite's when nested, and
// returns that base in `*outPtr`. The caller then writes the payload (if
// `hasValue`) via the matching JS::WriteCoerced* on `*step.child` at
// `*outPtr`.
extern JS_PUBLIC_API bool WriteCoercedOptionDiscriminant(
    WebIDLContext& ctx, const WebIDLCoerceStep& step, uint32_t memBase,
    bool hasValue, WasmScalarVal* result, uint32_t* outPtr);

// Writes a Variant-kind step's case discriminant: same self-allocation/
// `*outPtr` contract as WriteCoercedOptionDiscriminant, but for one of N
// cases. `caseIndex` is a source case index, not the target "kind" label
// index.
extern JS_PUBLIC_API bool WriteCoercedVariantCase(
    WebIDLContext& ctx, const WebIDLCoerceStep& step, uint32_t memBase,
    uint32_t caseIndex, WasmScalarVal* result, uint32_t* outPtr);

// Added (by the embedder, possibly more than once) to recognize an arbitrary
// JS value as a WebIDL interface reflector. Used only when resolving an
// imported resource type's WebIDL binding during instantiation; each
// registered hook is tried in registration order, and the first to return
// non-nullptr wins. Returns nullptr if `v` isn't a value the hook recognizes.
// If no hook recognizes `v`, imported resource types never resolve to an
// interface and fall back to the generic `object` coercion.
//
// Composable rather than a single slot: distinct embedders of the same
// process (e.g. Gecko's WebIDL bindings and the JS shell's testing
// functions, when testing functions are enabled inside Gecko) each add their
// own hook, recognizing disjoint value kinds.
using ResourceInterfaceFromValueFn = WebIDLInterface* (*)(JSContext * cx,
                                                          JS::HandleValue v);
extern JS_PUBLIC_API void AddResourceInterfaceFromValueHook(
    ResourceInterfaceFromValueFn fn);

}  // namespace JS

#endif  // ENABLE_WASM_COMPONENTS

// WebIDLValue is a compound stack type that traces its nested GC edges;
// Rooted<WebIDLValue> uses the default StructGCPolicy, which forwards to
// WebIDLValue::trace.

#endif  // js_experimental_WebIDL_h
