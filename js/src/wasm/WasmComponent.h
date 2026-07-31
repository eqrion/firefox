/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef wasm_component_h
#define wasm_component_h

#ifdef ENABLE_WASM_COMPONENTS

#  include "js/WasmComponent.h"

#  include "mozilla/Attributes.h"
#  include "mozilla/HashTable.h"
#  include "mozilla/Maybe.h"
#  include "mozilla/RefPtr.h"
#  include "mozilla/Span.h"
#  include "mozilla/UniquePtr.h"
#  include "mozilla/Variant.h"
#  include "mozilla/Vector.h"

#  include <type_traits>

#  include "gc/Barrier.h"
#  include "jit/IonTypes.h"
#  include "js/CallArgs.h"
#  include "js/experimental/WebIDL.h"
#  include "js/TracingAPI.h"
#  include "js/Wrapper.h"
#  include "wasm/WasmModule.h"

namespace js {
namespace wasm {

// A helper macro allowing component names to be printed with `%.*s`. Component
// names are always ASCII, so this is safe.
#  define ComponentName_Printf(n) \
    (int)(n).utf8Bytes().Length(), (n).utf8Bytes().data()

// A "sort", or "kind", of item in the component model, used for all cases where
// we must refer to a different item.
//
// This type is also used for the `externdesc` type, which describes what
// components (not core modules) can import and export, and whose cases are a
// subset of `sort`. Sorts that are valid for `externdesc` have the highest bit
// set. Additionally, sorts that can be exported by core modules (core:sort)
// have the second-highest bit set, and correspond to wasm::DefinitionKind.
// The bracket tag on an import/export name: `[constructor]r`, `[method]r.m`,
// `[static]r.m`, or an untagged plain name. Web.md's "Read the imports object"
// resolves the tagged forms against the resource type's JS constructor, and
// `canon lower` consults the tag again to decide how to invoke the callee.
enum class ComponentNameKind : uint8_t { Plain, Constructor, Method, Static };

enum class ComponentSort : uint8_t {
  Invalid = 0,

  Func = 0x80 | 0x01,
  Type = 0x80 | 0x03,
  Component = 0x80 | 0x04,
  Instance = 0x80 | 0x05,

  CoreFunction = 0x40 | int(DefinitionKind::Function),
  CoreTable = 0x40 | int(DefinitionKind::Table),
  CoreMemory = 0x40 | int(DefinitionKind::Memory),
  CoreGlobal = 0x40 | int(DefinitionKind::Global),
  CoreTag = 0x40 | int(DefinitionKind::Tag),

  CoreType = 0x10,
  CoreModule = 0x80 | 0x11,
  CoreInstance = 0x12,
};

// Checks if the given sort is valid for a component import or export (the
// component `externdesc` type).
inline bool ComponentSortValidForExternDesc(ComponentSort sort) {
  return (uint8_t(sort) & 0x80) != 0;
}

// Checks if the given sort is for a core item that can be imported or exported,
// i.e. a DefinitionKind imported into the component model. To extract the
// underlying DefinitionKind, use CoreSortFromComponentSort.
inline bool ComponentSortIsCoreSort(ComponentSort sort) {
  return (uint8_t(sort) & 0x40) != 0;
}

// Extracts the underlying DefinitionKind from a ComponentSort (if there is
// one).
inline DefinitionKind CoreSortFromComponentSort(ComponentSort sort) {
  MOZ_ASSERT(ComponentSortIsCoreSort(sort));
  return DefinitionKind(uint8_t(sort) & ~0xc0);
}

// ComponentTypeKind itself lives in the public js/WasmComponent.h; everything
// else about the component-model type system stays internal.

// Checks if the given kind is for a primitive type (`primvaltype`), i.e. one
// that doesn't need to be defined and referenced.
inline bool ComponentTypeKindIsPrimitive(ComponentTypeKind kind) {
  return ComponentTypeKind::FirstPrimitive <= kind &&
         kind <= ComponentTypeKind::LastPrimitive;
}

// Checks if the given kind is for a value type (`valtype`), i.e. one that can
// be used for function parameters.
inline bool ComponentTypeKindIsValueType(ComponentTypeKind kind) {
  return ComponentTypeKindIsPrimitive(kind) ||
         (ComponentTypeKind::Borrow <= kind &&
          kind <= ComponentTypeKind::Record &&
          int(kind) != 0x6c  // the one weird gap in the binary
         );
}

// Forward declarations to satisfy the methods in ComponentType
class ComponentTypeDef;
class ComponentType;
struct ComponentRecordField;
struct ComponentVariantCase;
struct ComponentResultType;
struct ComponentFuncType;
class ComponentResourceType;
using ComponentTypeVector =
    mozilla::Vector<ComponentType, 0, SystemAllocPolicy>;
using ComponentRecordFieldVector =
    mozilla::Vector<ComponentRecordField, 0, SystemAllocPolicy>;
using ComponentVariantCaseVector =
    mozilla::Vector<ComponentVariantCase, 0, SystemAllocPolicy>;

// The type of an item within a component.
class ComponentType {
  // TODO(wasm-cm): See if we could do a fancy tagging scheme to store the kind
  // in the bits of the pointer. It's a bit funky because right now we use high
  // bits in the kind for various purposes and so we can't pack it down into 3
  // or 4 bits like you'd want.
  ComponentTypeKind kind_;

  RefPtr<ComponentTypeDef> typeDef_;

  explicit ComponentType(ComponentTypeKind kind)
      : kind_(kind), typeDef_(nullptr) {
    MOZ_ASSERT(ComponentTypeKindIsPrimitive(kind));
  }
  explicit ComponentType(ComponentTypeKind kind,
                         RefPtr<ComponentTypeDef> typeDef)
      : kind_(kind), typeDef_(std::move(typeDef)) {}

 public:
  ComponentType() : kind_(ComponentTypeKind::Invalid), typeDef_(nullptr) {}
  bool isValid() const { return kind_ != ComponentTypeKind::Invalid; }

  // "Constructors" for various kinds of types. The resulting types will NOT be
  // canonical until added to the process-wide ComponentCanonicalTypeSet.
  static ComponentType primitive(ComponentTypeKind kind) {
    MOZ_RELEASE_ASSERT(ComponentTypeKindIsPrimitive(kind));
    return ComponentType(kind);
  }
  static bool record(ComponentRecordFieldVector&& fields, ComponentType* type);
  static bool variant(ComponentVariantCaseVector&& cases, ComponentType* type);
  static bool list(ComponentType&& elemType, ComponentType* type);
  static bool tuple(ComponentTypeVector&& items, ComponentType* type);
  static bool flags(CacheableNameVector&& labels, ComponentType* type);
  static bool enum_(CacheableNameVector&& cases, ComponentType* type);
  static bool option(ComponentType&& inner, ComponentType* type);
  static bool result(ComponentResultType&& inner, ComponentType* type);
  static bool own(ComponentType&& inner, ComponentType* type);
  static bool borrow(ComponentType&& inner, ComponentType* type);
  static bool func(ComponentFuncType&& inner, ComponentType* type);
  static bool resource(ComponentResourceType&& inner, ComponentType* type);
  static bool subResource(ComponentType* type);

  // Reconstructs a ComponentType from the raw (kind, typeDef) parts stored by
  // a public JS::WebIDLCoerceStep, which can't hold a ComponentType by value.
  static ComponentType fromKindAndTypeDef(ComponentTypeKind kind,
                                          RefPtr<ComponentTypeDef> typeDef) {
    return ComponentType(kind, std::move(typeDef));
  }

  ComponentTypeKind kind() const { return kind_; }
  RefPtr<ComponentTypeDef> typeDef() const { return typeDef_; }

  // Non-owning: for comparisons where the caller can guarantee typeDef_'s
  // owning Component outlives the use, without the atomic refcount traffic
  // of a RefPtr copy (see ComponentResourceTypeId).
  ComponentTypeDef* rawTypeDef() const { return typeDef_.get(); }

  const ComponentRecordFieldVector& asRecord() const;
  const ComponentVariantCaseVector& asVariant() const;
  ComponentType asList() const;
  const ComponentTypeVector& asTuple() const;
  const CacheableNameVector& asFlags() const;
  const CacheableNameVector& asEnum() const;
  ComponentType asOption() const;
  ComponentResultType asResult() const;
  ComponentType asOwn() const;
  ComponentType asBorrow() const;
  const ComponentFuncType& asFunc() const;
  const ComponentResourceType& asResource() const;

  // Only valid for an imported resource type (`sub resource`): binds the
  // WebIDL interface resolved from the import object during instantiation.
  // A locally-defined resource's interface is fixed at construction instead.
  void setSubResourceInterface(JS::WebIDLInterface* iface) const;

  // Cheaply checks if two canonicalized component types are equal under the
  // rules of the component model. This is fully general and handles resource
  // types, but because it compares ComponentTypeDef pointers for equality, only
  // canonicalized types are supported.
  bool operator==(const ComponentType& other) const {
    return kind_ == other.kind_ && typeDef_ == other.typeDef_;
  }
  static bool maybeEquals(mozilla::Maybe<ComponentType> a,
                          mozilla::Maybe<ComponentType> b) {
    if (a.isNothing() && b.isNothing()) {
      return true;
    }
    if (a.isSome() != b.isSome()) {
      return false;
    }
    return *a == *b;
  }

  // Checks if two (non-canonical) component types are structurally equal. This
  // is different from the usual `==` operator, which assumes types have been
  // canonicalized. Resource types will always come back as unequal.
  //
  // In almost all cases, the `==` operator is what you want.
  static bool structurallyEqual(const ComponentType& a, const ComponentType& b);
};

static_assert(std::is_default_constructible_v<ComponentType>);
static_assert(std::is_copy_constructible_v<ComponentType>);

struct ComponentTypeHasher {
  using Key = ComponentType;
  using Lookup = ComponentType;

  static HashNumber hash(const Lookup& aLookup);
  static bool match(const Key& aKey, const Lookup& aLookup);
};

struct ComponentCanonicalTypeSet {
  mozilla::HashSet<ComponentType, ComponentTypeHasher, SystemAllocPolicy>
      canonicalTypes_;

  bool canonicalize(const ComponentType& type, ComponentType* canonicalized);
};

// Canonicalizes `type` against the process-wide canonical type set, returning
// the canonical representative through `*canonicalized`. Thread-safe.
[[nodiscard]] bool CanonicalizeComponentType(const ComponentType& type,
                                             ComponentType* canonicalized);

// Empties the process-wide canonical type set. Intended for shutdown / testing.
void PurgeComponentCanonicalTypes();

struct ComponentRecordField {
  CacheableName name;
  ComponentType type;

  ComponentRecordField(CacheableName&& name_, ComponentType type_)
      : name(std::move(name_)), type(type_) {}

  bool operator==(const ComponentRecordField& other) const {
    return name == other.name && type == other.type;
  }
};

struct ComponentVariantCase {
  CacheableName name;
  mozilla::Maybe<ComponentType> type;

  bool operator==(const ComponentVariantCase& other) const {
    return name == other.name && ComponentType::maybeEquals(type, other.type);
  }
};

struct ComponentResultType {
  mozilla::Maybe<ComponentType> type;
  mozilla::Maybe<ComponentType> errorType;

  static bool equals(const ComponentResultType& a,
                     const ComponentResultType& b) {
    return ComponentType::maybeEquals(a.type, b.type) &&
           ComponentType::maybeEquals(a.errorType, b.errorType);
  }
};

struct ComponentFuncType {
  ComponentTypeVector paramTypes;
  CacheableNameVector paramNames;
  mozilla::Maybe<ComponentType> resultType;

  bool operator==(const ComponentFuncType& other) const {
    MOZ_RELEASE_ASSERT(paramTypes.length() == paramNames.length());
    MOZ_RELEASE_ASSERT(other.paramTypes.length() == other.paramNames.length());
    if (paramTypes.length() != other.paramTypes.length()) {
      return false;
    }

    for (size_t i = 0; i < paramTypes.length(); i++) {
      if (paramTypes[i] != other.paramTypes[i] ||
          paramNames[i] != other.paramNames[i]) {
        return false;
      }
    }

    if (!ComponentType::maybeEquals(resultType, other.resultType)) {
      return false;
    }

    return true;
  }
};

class ComponentResourceType {
  // All resource types have (rep i32) for the time being.

  mozilla::Maybe<uint32_t> dtorIndex_;

  // The WebIDL interface this resource is declared as, if any. Null means the
  // resource has no WebIDL constructor and canonicalizes to the generic
  // `object` type instead.
  RefPtr<JS::WebIDLInterface> interface_;

 public:
  explicit ComponentResourceType(
      mozilla::Maybe<uint32_t> dtorIndex = mozilla::Nothing(),
      RefPtr<JS::WebIDLInterface> interface = nullptr)
      : dtorIndex_(dtorIndex), interface_(std::move(interface)) {}

  mozilla::Maybe<uint32_t> dtorIndex() const { return dtorIndex_; }
  JS::WebIDLInterface* interface() const { return interface_; }

  // Only for an imported resource type (`sub resource`): binds the WebIDL
  // interface resolved from the import object during instantiation.
  void setInterface(JS::WebIDLInterface* iface) { interface_ = iface; }
};

using ComponentTypeSchema = mozilla::Variant<
    mozilla::Nothing, ComponentType, ComponentRecordFieldVector,
    ComponentVariantCaseVector, ComponentTypeVector, CacheableNameVector,
    ComponentResultType, ComponentFuncType, ComponentResourceType>;

class ComponentTypeDef : public AtomicRefCounted<ComponentTypeDef> {
  ComponentTypeSchema schema_;

 public:
  explicit ComponentTypeDef(ComponentTypeSchema&& schema)
      : schema_(std::move(schema)) {}

  const ComponentTypeSchema& schema() const { return schema_; }
  ComponentTypeSchema& mutableSchema() { return schema_; }

  // Checks two typedefs for structural equality. Note that this is NOT the same
  // as comparing two types for equality, because a) not all types even have
  // ComponentTypeDefs, b) different type kinds may share the same kind of
  // backing storage (e.g. flags and enums), and c) because this method always
  // considers resource types to be unequal.
  static bool structurallyEqual(const ComponentTypeDef& a,
                                const ComponentTypeDef& b);
};

// A non-owning (kind, typeDef) identity for a resource type - cheap to copy,
// store, and compare, unlike ComponentType, since it never touches
// ComponentTypeDef's atomic refcount. Safe wherever the type's owning
// Component is already guaranteed to outlive the use: every resource type's
// ComponentTypeDef (including a `sub resource` import placeholder) is
// allocated into its declaring Component's own definedTypes_, and a
// ComponentInstance holds a strong SharedComponent for its entire lifetime -
// destroyed only after its ComponentHandleTable, per C++'s reverse
// member-declaration-order destruction - so a resource handle can never
// outlive the Component that owns its type.
class ComponentResourceTypeId {
  ComponentTypeKind kind_ = ComponentTypeKind::Invalid;
  ComponentTypeDef* typeDef_ = nullptr;

 public:
  ComponentResourceTypeId() = default;
  ComponentResourceTypeId(ComponentTypeKind kind, ComponentTypeDef* typeDef)
      : kind_(kind), typeDef_(typeDef) {}
  explicit ComponentResourceTypeId(const ComponentType& type)
      : kind_(type.kind()), typeDef_(type.rawTypeDef()) {}

  ComponentTypeKind kind() const { return kind_; }
  ComponentTypeDef* typeDef() const { return typeDef_; }

  bool operator==(const ComponentResourceTypeId& other) const {
    return kind_ == other.kind_ && typeDef_ == other.typeDef_;
  }
  bool operator!=(const ComponentResourceTypeId& other) const {
    return !(*this == other);
  }
};

// Classifies an incoming JS object as `Native` (a bare pointer, unwrapped via
// its interface, for the fast same-origin case) or `Wrapped` (a plain object
// reference: the fallback for cross-origin values, or when there's no way to
// unwrap to a bare pointer). Doesn't take any ref; ComponentHandleTable does
// that itself when the classification is stored, unless `adoptRef` is set (see
// AdoptNative), in which case the caller's existing ref transfers instead.
struct ComponentResourceClassification {
  bool isNative;
  void* nativePtr;                   // valid iff isNative
  JS::WebIDLInterface* nativeIface;  // valid iff isNative
  JSObject* wrappedObj;              // valid iff !isNative
  bool adoptRef = false;             // valid iff isNative

  // A Native classification for a pointer the caller already holds a strong
  // ref on (e.g. a WebIDL native call result) - ComponentHandleTable::add
  // takes over that ref instead of calling addRefNative itself.
  static ComponentResourceClassification AdoptNative(
      void* aNative, JS::WebIDLInterface* aIface) {
    return {true, aNative, aIface, nullptr, true};
  }
};

inline ComponentResourceClassification ClassifyResourceObject(
    JSObject* obj, bool isSameOrigin, JS::WebIDLInterface* iface) {
  if (iface && isSameOrigin && iface->hasNativeOps() &&
      iface->canUnwrapNative()) {
    if (void* native = iface->unwrapNative(obj)) {
      return {true, native, iface, nullptr};
    }
  }
  JSObject* wrapped = (iface && isSameOrigin) ? UncheckedUnwrap(obj) : obj;
  return {false, nullptr, nullptr, wrapped};
}

// A non-owning snapshot of a ComponentHandleTable entry: the spec's
// `ResourceHandle`. `own` distinguishes an owning handle from a borrowed one;
// `numLends` must be zero for an `own` handle to be lifted or dropped.
// Trivially copyable - reading one (via ComponentHandleTable::get) never
// changes the table's ownership of whatever ref it holds.
// ComponentHandleTable::remove is the one operation that transfers that
// ownership to the caller; use ComponentResourceHandleOwner, below, to hold
// the result of a remove() so the ref is released exactly once regardless of
// which return path is taken.
struct ComponentResourceHandle {
  ComponentResourceTypeId resourceType;
  bool own = false;
  uint32_t numLends = 0;
  bool isNative = false;
  void* nativePtr = nullptr;                   // valid iff isNative
  JS::WebIDLInterface* nativeIface = nullptr;  // valid iff isNative
  JSObject* wrappedObj = nullptr;              // valid iff !isNative

  // Defined out of line in WasmComponent.cpp: needs JS_WrapObject, and
  // jsapi.h isn't includable from this header.
  JSObject* reflectToJS(JSContext* cx) const;

  // Resolves to a native pointer either directly (isNative) or by unwrapping
  // the wrapped object through `expectedInterface`, if given.
  void* resolveNative(JS::WebIDLInterface* expectedInterface) const {
    if (isNative) {
      return nativePtr;
    }
    return expectedInterface ? expectedInterface->unwrapNative(wrappedObj)
                             : nullptr;
  }

  // Releases the strong ref this handle holds, if any. Only meaningful after
  // ComponentHandleTable::remove, which transfers that ownership here.
  void release() const {
    if (isNative) {
      nativeIface->releaseNative(nativePtr);
    }
  }
};

// RAII wrapper for the result of ComponentHandleTable::remove: releases the
// removed handle's ref (if any) exactly once, on every return path, the way
// the pre-refactor ComponentResourceHandle's destructor used to.
class ComponentResourceHandleOwner {
  ComponentResourceHandle handle_;

 public:
  explicit ComponentResourceHandleOwner(const ComponentResourceHandle& handle)
      : handle_(handle) {}
  ComponentResourceHandleOwner(const ComponentResourceHandleOwner&) = delete;
  ComponentResourceHandleOwner& operator=(const ComponentResourceHandleOwner&) =
      delete;
  ~ComponentResourceHandleOwner() { handle_.release(); }

  const ComponentResourceHandle* operator->() const { return &handle_; }
  const ComponentResourceHandle& get() const { return handle_; }

  // Transfers the handle's strong ref (if any) to the caller: the destructor
  // no longer releases it. Used when the caller wants to keep the ref alive
  // past this call rather than have it released here.
  void forget() { handle_.isNative = false; }
};

// A table of resource handles, mirroring the component model spec's generic
// `Table`. Index 0 is reserved as a sentinel and never allocated.
//
// Storage is a hand-rolled, trivially-copyable Entry array (plus a parallel
// free-index stack) rather than mozilla::Vector<Maybe<...>>, so that JIT code
// inlining resource.drop can read the table's geometry and an entry's fields
// through static, self-owned offset accessors instead of reverse-engineering
// Vector/Maybe/Variant layout. Entries move on growth: JIT code must load
// `entries_`/`freeStack_` fresh on every access, never bake them.
class ComponentHandleTable {
 public:
  enum : uint32_t {
    Present = 1u << 0,
    Own = 1u << 1,
    // Set iff the entry's value is a native pointer (`ptr`/`iface` valid);
    // else the value is the JSObject* at the same index in `wrapped_`.
    Native = 1u << 2,
  };
  static constexpr uint32_t KindShift = 8;
  static constexpr uint32_t KindMask = 0xffu << KindShift;

  // A single table entry. Trivially copyable: grown via js_pod_realloc and
  // moved with memmove, never via element-wise construction. Does not itself
  // hold a JSObject* - Wrapped values live in the table's parallel `wrapped_`
  // array, so this struct (the one the JIT touches) never needs a GC barrier.
  struct Entry {
    uint32_t flags = 0;
    uint32_t numLends = 0;
    void* ptr = nullptr;                   // valid iff Native
    JS::WebIDLInterface* iface = nullptr;  // valid iff Native
    ComponentTypeDef* resourceTypeDef = nullptr;

    static constexpr size_t offsetOfFlags() { return offsetof(Entry, flags); }
    static constexpr size_t offsetOfNumLends() {
      return offsetof(Entry, numLends);
    }
    static constexpr size_t offsetOfPtr() { return offsetof(Entry, ptr); }
    static constexpr size_t offsetOfIface() { return offsetof(Entry, iface); }
  };
  static_assert(std::is_trivially_copyable_v<Entry>);
  // MaxLength, below, assumes this fits comfortably in the JIT's Int32
  // `index * sizeof(Entry)` address computation - grow MaxLength's safety
  // margin if this ever grows past 32.
  static_assert(sizeof(Entry) <= 32);

 private:
  Entry* entries_ = nullptr;
  uint32_t length_ = 0;
  uint32_t capacity_ = 0;
  uint32_t* freeStack_ = nullptr;
  uint32_t freeLength_ = 0;
  uint32_t freeCapacity_ = 0;
  // Wrapped handles' JSObject*s, 1:1 with entries_ by index (same length,
  // grown in lockstep). Kept out of Entry so the JIT-visible entry stays POD;
  // wrapped handles are the cold, non-native-fast-path case.
  mozilla::Vector<JS::Heap<JSObject*>, 0, SystemAllocPolicy> wrapped_;

  // Capped well below the point where `index * sizeof(Entry)` (computed in
  // Int32 by the JIT's inlined resource.drop fast path) could overflow: at
  // 32 bytes/entry, 2^20 entries is a 32 MiB table, orders of magnitude past
  // any real workload, while keeping the product under 2^31.
  static constexpr uint32_t MaxLength = 1u << 20;

  [[nodiscard]] bool growEntries(JSContext* cx);
  [[nodiscard]] bool pushFree(JSContext* cx, uint32_t index);
  [[nodiscard]] bool reserve(JSContext* cx, uint32_t* index);

 public:
  ComponentHandleTable() = default;
  ~ComponentHandleTable();

  static constexpr size_t offsetOfEntries() {
    return offsetof(ComponentHandleTable, entries_);
  }
  static constexpr size_t offsetOfLength() {
    return offsetof(ComponentHandleTable, length_);
  }
  static constexpr size_t offsetOfFreeStack() {
    return offsetof(ComponentHandleTable, freeStack_);
  }
  static constexpr size_t offsetOfFreeLength() {
    return offsetof(ComponentHandleTable, freeLength_);
  }
  static constexpr size_t offsetOfFreeCapacity() {
    return offsetof(ComponentHandleTable, freeCapacity_);
  }

  [[nodiscard]] bool add(JSContext* cx, ComponentResourceTypeId resourceType,
                         bool own, const ComponentResourceClassification& value,
                         uint32_t* index);
  [[nodiscard]] bool get(JSContext* cx, uint32_t index,
                         ComponentResourceHandle* out) const;
  // Direct access to a present entry, for hot paths (e.g. a borrow read)
  // that don't need the full ComponentResourceHandle snapshot. Null iff
  // `index` is out of range or free; the caller reports the trap. The
  // returned pointer is invalidated by any subsequent add().
  const Entry* peek(uint32_t index) const {
    if (index >= length_ || !(entries_[index].flags & Present)) {
      return nullptr;
    }
    return &entries_[index];
  }
  JSObject* wrappedAt(uint32_t index) const {
    if (index >= wrapped_.length()) {
      return nullptr;
    }
    return wrapped_[index];
  }
  // Marks `index` free and transfers ownership of any strong ref it holds to
  // `out`; the caller must call out->release() once done with it (see
  // ComponentResourceHandle's comment).
  [[nodiscard]] bool remove(JSContext* cx, uint32_t index,
                            ComponentResourceHandle* out);
  [[nodiscard]] bool drop(JSContext* cx, uint32_t index, bool requireOwn);

  size_t count() const {
    size_t n = 0;
    for (uint32_t i = 0; i < length_; i++) {
      if (entries_[i].flags & Present) {
        n++;
      }
    }
    return n;
  }

  void trace(JSTracer* trc) {
    for (auto& obj : wrapped_) {
      if (obj) {
        JS::TraceEdge(trc, &obj, "ComponentHandleTable wrapped resource");
      }
    }
  }
};

class ComponentValue;
class ComponentValueData;
using ComponentValueVector =
    mozilla::Vector<ComponentValue, 0, SystemAllocPolicy>;

// A component-model value. Holds no JS GC pointers (strings are native
// UTF-8, resource handles are integers), so no tracing is needed. Cheaply
// copyable: scalars by value, composites share a refcounted
// ComponentValueData.
class ComponentValue {
  using Repr = mozilla::Variant<mozilla::Nothing, bool, int64_t, uint64_t,
                                float, double, RefPtr<ComponentValueData>>;

  ComponentType type_;
  Repr repr_ = Repr(mozilla::Nothing());

  ComponentValue(const ComponentType& type, Repr&& repr)
      : type_(type), repr_(std::move(repr)) {}

 public:
  ComponentValue() = default;

  const ComponentType& type() const { return type_; }
  ComponentTypeKind kind() const { return type_.kind(); }
  bool isValid() const { return type_.isValid(); }

  static ComponentValue makeBool(const ComponentType& type, bool b);
  static ComponentValue makeSigned(const ComponentType& type, int64_t v);
  static ComponentValue makeUnsigned(const ComponentType& type, uint64_t v);
  static ComponentValue makeF32(const ComponentType& type, float v);
  static ComponentValue makeF64(const ComponentType& type, double v);
  static ComponentValue makeComposite(const ComponentType& type,
                                      RefPtr<ComponentValueData> data);

  bool asBool() const { return repr_.as<bool>(); }
  int64_t asSigned() const { return repr_.as<int64_t>(); }
  uint64_t asUnsigned() const { return repr_.as<uint64_t>(); }
  float asF32() const { return repr_.as<float>(); }
  double asF64() const { return repr_.as<double>(); }
  ComponentValueData* data() const {
    return repr_.as<RefPtr<ComponentValueData>>();
  }
};

static_assert(std::is_copy_constructible_v<ComponentValue>);

// The payload backing a composite ComponentValue.
class ComponentValueData : public AtomicRefCounted<ComponentValueData> {
 public:
  struct StringData {
    mozilla::Vector<char, 0, SystemAllocPolicy> utf8;
  };
  // list / record (aligned to fields) / tuple.
  struct ElementsData {
    ComponentValueVector elements;
  };
  struct FlagsData {
    mozilla::Vector<bool, 0, SystemAllocPolicy> bits;  // aligned to labels
  };
  // option: `payload` empty means `none`.
  struct OptionData {
    mozilla::Maybe<ComponentValue> payload;
  };
  struct ResultData {
    bool isOk;
    mozilla::Maybe<ComponentValue> payload;
  };
  struct VariantData {
    uint32_t caseIndex;
    mozilla::Maybe<ComponentValue> payload;
  };

 private:
  mozilla::Variant<StringData, ElementsData, FlagsData, OptionData, ResultData,
                   VariantData>
      data_;

 public:
  template <typename T>
  explicit ComponentValueData(T&& d) : data_(std::move(d)) {}

  const StringData& asString() const { return data_.as<StringData>(); }
  const ElementsData& asElements() const { return data_.as<ElementsData>(); }
  const FlagsData& asFlags() const { return data_.as<FlagsData>(); }
  const OptionData& asOption() const { return data_.as<OptionData>(); }
  const ResultData& asResult() const { return data_.as<ResultData>(); }
  const VariantData& asVariant() const { return data_.as<VariantData>(); }

  ElementsData& mutableElements() { return data_.as<ElementsData>(); }
};

// The canonical WebIDL type representing a component value type. Returns an
// invalid WebIDLType on OOM.
[[nodiscard]] bool CanonicalWebIDLType(JSContext* cx,
                                       const ComponentType& componentType,
                                       JS::WebIDLType* out);

// Converts a component value to its canonical WebIDL value. `handles` is
// required if `value`'s type transitively contains an `own`/`borrow` handle.
[[nodiscard]] bool ToCanonicalWebIDLValue(
    JSContext* cx, const ComponentValue& value,
    JS::MutableHandleWebIDLValue out, ComponentHandleTable* handles = nullptr);

// The inverse of ToCanonicalWebIDLValue.
[[nodiscard]] bool FromCanonicalWebIDLValue(
    JSContext* cx, JS::HandleWebIDLValue value, const ComponentType& targetType,
    ComponentValue* out, ComponentHandleTable* handles = nullptr);

// The canonical WebIDL operation signature for a component function type: its
// param types and (optional) return type, each mapped through
// CanonicalWebIDLType.
[[nodiscard]] bool CanonicalWebIDLOperationType(
    JSContext* cx, const ComponentFuncType& funcType,
    JS::WebIDLOperationType* out);

class Component;

enum class CanonMode : uint8_t {
  Lift,
  Lower,
};

[[nodiscard]] bool FlattenTypes(const ComponentTypeVector& types,
                                ValTypeVector* result, bool* hasStringsOrLists,
                                bool* tooDeep, uint32_t depth);
[[nodiscard]] bool FlattenType(const ComponentType& type, ValTypeVector* result,
                               bool* hasStringsOrLists, bool* tooDeep,
                               uint32_t depth);
[[nodiscard]] bool FlattenRecord(const ComponentRecordFieldVector& fields,
                                 ValTypeVector* result, bool* hasStringsOrLists,
                                 bool* tooDeep, uint32_t depth);
mozilla::Maybe<FuncType> FlattenFuncType(const ComponentFuncType& funcType,
                                         CanonMode mode, bool* memoryRequired,
                                         bool* reallocRequired, bool* tooDeep);

// Convenience wrappers for callers that just need the flattened shape of an
// already-validated type, where the string/list tracking and depth limit
// checked by validation don't apply.
[[nodiscard]] bool FlattenTypes(const ComponentTypeVector& types,
                                ValTypeVector* result);
[[nodiscard]] bool FlattenType(const ComponentType& type,
                               ValTypeVector* result);

// A func whose flattened parameter/result count exceeds these limits instead
// passes a single pointer to a tuple written into linear memory.
constexpr uint32_t MaxFlatParams = 16;
constexpr uint32_t MaxFlatResults = 1;

// Aliases dropping the verbose `JS::WebIDL` prefix for this file's internal
// use.
using CoerceOp = JS::WebIDLCoerceOp;
using StepKind = JS::WebIDLStepKind;
using AccessLoc = JS::WebIDLAccessLoc;
using DictMemberStep = JS::WebIDLDictMemberStep;
using DictMemberStepVector = JS::WebIDLDictMemberStepVector;
using VariantCaseStep = JS::WebIDLVariantCaseStep;
using VariantCaseStepVector = JS::WebIDLVariantCaseStepVector;
using CoerceStep = JS::WebIDLCoerceStep;
using ParamStep = JS::WebIDLParamStep;
using ParamStepVector = JS::WebIDLParamStepVector;
using CallAdapter = JS::WebIDLCallAdapter;
using JS::WebIDLStringKind;

// Given the overload set for a WebIDL operation and the component function's
// fixed arity, picks the overload whose declared parameter class matches the
// component's canonical class. `*outIndex` is Nothing if no candidate
// matches; returns false only on OOM.
[[nodiscard]] bool SelectWebIDLOverload(
    JSContext* cx, const ComponentFuncType& componentFuncType,
    const JS::WebIDLOperationTypeVector& overloads,
    mozilla::Maybe<size_t>* outIndex);

// Compiles `(componentFuncType, webIDLOpType)` into a `CallAdapter`. Always
// succeeds - a statically-doomed binding comes back with `isTrap = true`
// rather than an error. `out` is value-initialized (fresh) on entry.
[[nodiscard]] bool BuildCallAdapter(
    JSContext* cx, const ComponentFuncType& componentFuncType,
    const JS::WebIDLOperationType& webIDLOpType,
    ComponentStringEncoding instanceStringEncoding, CallAdapter* out);

// The reverse direction: coerces a WebIDL return value of type
// `webIDLReturnType` into a component value of `componentResultType`.
[[nodiscard]] bool BuildResultStep(JSContext* cx,
                                   const JS::WebIDLType& webIDLReturnType,
                                   const ComponentType& componentResultType,
                                   CoerceStep* out);

// Implements `canon lift`: calls `coreCallee` (a core wasm export, exposed as
// a JS-callable function) with `args`, converting JS values <-> WebIDL
// values <-> component values <-> core wasm values along the way.
// `postReturn`, if not undefined, is called with the raw core result
// value(s) once every byte needed from them has been copied out. Any
// own/borrow handle is resolved through `abiCtx.handles()`. A top-level
// result<T,E> return type's error(e) case throws the raw payload value
// directly (result<T,E> throwing isn't otherwise wired up yet).
[[nodiscard]] bool LiftedCall(JSContext* cx, JS::HandleObject coreCallee,
                              const ComponentFuncType& funcType,
                              JS::WebIDLContext& abiCtx,
                              JS::HandleValue postReturn,
                              const JS::CallArgs& args);

// A hash policy for StronglyUniqueNameSet that hashes items based on their
// trimmed, lowercased versions, but matches based on the full strongly-unique
// rules.
//
// The full strongly-unique rules are not hash-friendly; we have not yet figured
// out any way to "normalize" the name to a unique key that satisfies the
// strange carve-out rules for constructor and method names. But, we don't want
// to quadratically check each new name against every other name, so we take a
// disappointing halfway approach of hashing only the base part of the name, and
// then running the full strongly-unique logic in `match`. This results in more
// hash collisions and a less-inexpensive `match` method, but at least it keeps
// things from growing quadratically.
struct StronglyUniqueNameHasher {
  using Key = CacheableName;
  using Lookup = mozilla::Span<const char>;

  static HashNumber hash(const Lookup& aLookup);
  static bool match(const Key& aKey, const Lookup& aLookup);
};

// A class which can be used to check if a set of component model names is
// strongly-unique. The set owns its keys.
class StronglyUniqueNameSet {
  mozilla::HashSet<CacheableName, StronglyUniqueNameHasher, SystemAllocPolicy>
      data_;

 public:
  [[nodiscard]] bool add(mozilla::Span<const char> name, bool* duplicate);
};

struct ComponentCanonOpts {
  ComponentStringEncoding stringEncoding;
  mozilla::Maybe<uint32_t> memoryIndex;
  mozilla::Maybe<uint32_t> reallocIndex;
  mozilla::Maybe<uint32_t> postReturnIndex;
};

class ComponentLiftedFuncDesc {
  uint32_t coreFuncIndex_;
  uint32_t typeIndex_;
  ComponentCanonOpts canonOpts_;

 public:
  ComponentLiftedFuncDesc(uint32_t coreFuncIndex, uint32_t typeIndex,
                          ComponentCanonOpts canonOpts)
      : coreFuncIndex_(coreFuncIndex),
        typeIndex_(typeIndex),
        canonOpts_(canonOpts) {}

  // The `coreFuncs()` index of the core function this `canon lift` wraps.
  uint32_t coreFuncIndex() const { return coreFuncIndex_; }

  // This returns the raw type index. To get the ComponentFuncType, call
  // Component::typeForFunc instead.
  uint32_t typeIndex() const { return typeIndex_; }
  const ComponentCanonOpts& canonOpts() const { return canonOpts_; }
};

// TODO(wasm-cm): Eventually this should probably be reworked to support all the
// async builtins too, but that is a future problem.
class ComponentResourceBuiltin {
 public:
  enum class Kind : uint8_t {
    ResourceNew = 0x02,  // aligns with binary encoding
    ResourceDrop = 0x03,
    ResourceRep = 0x04,
  };

 private:
  Kind kind_;
  ComponentType resourceType_;

 public:
  ComponentResourceBuiltin(Kind kind, ComponentType resourceType)
      : kind_(kind), resourceType_(resourceType) {
    // resource.new/rep only apply to a resource this component itself
    // defines; resource.drop applies to any resource type, including one
    // this component only imports (SubResource).
    MOZ_ASSERT(resourceType.kind() == ComponentTypeKind::Resource ||
               (kind == Kind::ResourceDrop &&
                resourceType.kind() == ComponentTypeKind::SubResource));
  }

  Kind kind() const { return kind_; }
  const ComponentType& resourceType() const { return resourceType_; }
};

// A class representing a component function (or other host function) lowered
// such that a core module can call it. This includes both (canon lower) and
// other things like builtin definitions (e.g. (canon resource.new)).
class ComponentCoreFuncDesc {
  using Payload = mozilla::Variant<uint32_t, ComponentResourceBuiltin>;

 private:
  SharedTypeDef coreFuncType_;
  Payload payload_;
  // Only meaningful for the `lowered` (uint32_t payload) variant - a resource
  // builtin has no canon opts of its own.
  ComponentCanonOpts canonOpts_;

  ComponentCoreFuncDesc(uint32_t funcIndex, SharedTypeDef&& flattenedType,
                        ComponentCanonOpts&& canonOpts)
      : coreFuncType_(std::move(flattenedType)),
        payload_(funcIndex),
        canonOpts_(std::move(canonOpts)) {}
  ComponentCoreFuncDesc(ComponentResourceBuiltin&& builtinDesc,
                        SharedTypeDef&& coreFuncType)
      : coreFuncType_(std::move(coreFuncType)),
        payload_(builtinDesc),
        canonOpts_{} {}

 public:
  static ComponentCoreFuncDesc lowered(uint32_t funcIndex,
                                       SharedTypeDef&& flattenedType,
                                       ComponentCanonOpts&& canonOpts) {
    return ComponentCoreFuncDesc(funcIndex, std::move(flattenedType),
                                 std::move(canonOpts));
  }
  static ComponentCoreFuncDesc builtin(ComponentResourceBuiltin&& builtinDesc,
                                       SharedTypeDef&& coreFuncType) {
    return ComponentCoreFuncDesc(std::move(builtinDesc),
                                 std::move(coreFuncType));
  }

  const Payload& payload() const { return payload_; }
  const SharedTypeDef& coreFuncType() const { return coreFuncType_; }
  // Only valid to call when `payload().is<uint32_t>()`.
  const ComponentCanonOpts& canonOpts() const { return canonOpts_; }
};

// A sort/index pair referring to an item within a component or core module,
// corresponding to `sortidx` or `core:sortidx` in the component model spec.
struct ComponentSortIndex {
  ComponentSort sort = ComponentSort::Invalid;
  uint32_t index = 0;

  ComponentSortIndex() = default;
  ComponentSortIndex(ComponentSort sort, uint32_t index)
      : sort(sort), index(index) {}

  bool operator==(const ComponentSortIndex& other) const {
    return sort == other.sort && index == other.index;
  }
};

struct ComponentSortIndexHasher {
  using Lookup = ComponentSortIndex;
  static HashNumber hash(const Lookup& l) {
    return mozilla::HashGeneric(l.sort, l.index);
  }
  static bool match(const ComponentSortIndex& k, const Lookup& l) {
    return k == l;
  }
};

enum class ComponentAliasKind : uint8_t {
  CoreExport,
  Export,
  Outer,
};

// A generalized reference to an item in the component model. A ComponentItem
// may reference an import, an export, an item defined in the component itself,
// or an alias to an item defined elsewhere. This is the main type used for each
// index space in the component model, as imports, exports, aliases, and defined
// items can be interleaved in any order.
//
// The data is stored into two fields, one of which identifies the index space
// for the item (possibly in another component), and the other of which is the
// index in that index space.
//
// This first field, whatAndWhere_, stores all the information necessary to find
// the index space for the item. It is a packed field laid out like so:
//
//     000 00000000 00 0000000000000000000
//     │   │        │  └ instance index (ItemKind::Alias only)
//     │   │        └ alias kind (type ComponentAliasKind, ItemKind::Alias only)
//     │   └ sort (type ComponentSort)
//     └ kind (type ItemKind)
//
// For all ItemKinds, we store the "sort" of the item (e.g. func, table, type,
// or core func). This is not strictly necessary for all kinds, but facilitates
// debugging and can catch bugs. It _is_ strictly necessary for ItemKind::Alias,
// which uses the `(core:)?sortidx` production from the component spec.
// Additionally, for ItemKind::Alias we store the ComponentAliasKind (core
// export alias, component export alias, or outer alias) and the instance index,
// which is the index of the core instance, component instance, or outer
// component to fetch an item from.
//
// The second field, itemIndex_, is simply a uint32_t item index like you'd find
// anywhere else.
class ComponentItem {
  uint32_t whatAndWhere_;
  uint32_t itemIndex_;

  friend struct ComponentItemHasher;

 public:
  static constexpr uint32_t ItemKindShift = 29;
  static constexpr uint32_t ItemKindMask = 0b111 << ItemKindShift;
  static constexpr uint32_t SortShift = 21;
  static constexpr uint32_t SortMask = 0b11111111 << SortShift;
  static constexpr uint32_t AliasKindShift = 19;
  static constexpr uint32_t AliasKindMask = 0b11 << AliasKindShift;
  static constexpr uint32_t AliasInstanceMask = (1 << AliasKindShift) - 1;

  enum class ItemKind : uint8_t {
    Invalid = 0,

    // For Defined, Import, and Export, the sort of the item is always clear
    // from context. For example, when looking up a function by index, you would
    // get an item from the component's `funcs_` vector; therefore, the only
    // things you need to know are whether it is defined, imported, or exported,
    // and what index it would be in each of those three relevant vectors.
    // However, we still redundantly store a sort on these items because we have
    // the space in `whatAndWhere_` and can use it to catch bugs.
    Defined,
    Import,
    Export,

    // Alias refers to the component concept of "alias"; that is, projecting an
    // item out of another component/core instance into the current instance's
    // index space. For this we require all fields of `whatAndWhere_`.
    Alias,
  };

  explicit ComponentItem(ItemKind kind, ComponentSort sort, uint32_t itemIndex)
      : whatAndWhere_(0), itemIndex_(itemIndex) {
    MOZ_ASSERT(kind != ItemKind::Alias);

    whatAndWhere_ |= uint32_t(kind) << ItemKindShift;
    whatAndWhere_ |= uint32_t(sort) << SortShift;

    MOZ_ASSERT(this->kind() == kind);
    MOZ_ASSERT(this->sort() == sort);
  }
  explicit ComponentItem(ComponentAliasKind aliasKind, ComponentSort sort,
                         uint32_t instanceIndex, uint32_t itemIndex)
      : whatAndWhere_(0), itemIndex_(itemIndex) {
    MOZ_ASSERT((instanceIndex & ~AliasInstanceMask) == 0);
    whatAndWhere_ |= uint32_t(ItemKind::Alias) << ItemKindShift;
    whatAndWhere_ |= uint32_t(sort) << SortShift;
    whatAndWhere_ |= uint32_t(aliasKind) << AliasKindShift;
    whatAndWhere_ |= instanceIndex;

    MOZ_ASSERT(this->kind() == ItemKind::Alias);
    MOZ_ASSERT(this->sort() == sort);
    MOZ_ASSERT(this->aliasKind() == aliasKind);
    MOZ_ASSERT(this->aliasInstanceIndex() == instanceIndex);
  }

 public:
  ComponentItem() : whatAndWhere_(0), itemIndex_(0) {
    MOZ_ASSERT(this->kind() == ItemKind::Invalid);
  }

  static ComponentItem defined(ComponentSort sort, uint32_t itemIndex) {
    return ComponentItem(ItemKind::Defined, sort, itemIndex);
  }
  static ComponentItem import(ComponentSort sort, uint32_t itemIndex) {
    return ComponentItem(ItemKind::Import, sort, itemIndex);
  }
  static ComponentItem export_(ComponentSort sort, uint32_t itemIndex) {
    return ComponentItem(ItemKind::Export, sort, itemIndex);
  }
  static ComponentItem alias(ComponentAliasKind aliasKind, ComponentSort sort,
                             uint32_t instanceIndex, uint32_t itemIndex) {
    return ComponentItem(aliasKind, sort, instanceIndex, itemIndex);
  }

  ItemKind kind() const {
    return ItemKind((whatAndWhere_ & ItemKindMask) >> ItemKindShift);
  }
  ComponentSort sort() const {
    return ComponentSort((whatAndWhere_ & SortMask) >> SortShift);
  }
  uint32_t itemIndex() const { return itemIndex_; }

  ComponentAliasKind aliasKind() const {
    MOZ_RELEASE_ASSERT(kind() == ItemKind::Alias);
    return ComponentAliasKind((whatAndWhere_ & AliasKindMask) >>
                              AliasKindShift);
  }
  uint32_t aliasInstanceIndex() const {
    MOZ_RELEASE_ASSERT(kind() == ItemKind::Alias);
    return whatAndWhere_ & AliasInstanceMask;
  }

  // Outer aliases are a fun special case that comes up often enough to deserve
  // a short helper.
  bool isOuterAlias() const {
    return kind() == ItemKind::Alias &&
           aliasKind() == ComponentAliasKind::Outer;
  }
  ComponentSortIndex outerAliasSortIndex() const {
    MOZ_RELEASE_ASSERT(isOuterAlias());
    return ComponentSortIndex(sort(), itemIndex());
  }

  bool operator==(const ComponentItem& other) const {
    return whatAndWhere_ == other.whatAndWhere_ &&
           itemIndex_ == other.itemIndex_;
  }
};

// TODO(wasm-cm): Add static asserts for MaxComponents and
// MaxComponentNestingDepth or whatever, eventually
static_assert(MaxComponentCoreInstances <= ComponentItem::AliasInstanceMask);

struct ComponentItemHasher {
  using Lookup = ComponentItem;
  static HashNumber hash(const Lookup& l) {
    return mozilla::HashGeneric(l.whatAndWhere_, l.itemIndex_);
  }
  static bool match(const ComponentItem& k, const Lookup& l) { return k == l; }
};

using CoreInstanceInstantiateArgs =
    mozilla::HashMap<CacheableName,  // import module name
                     uint32_t,       // instance index
                     CacheableNameHasher, SystemAllocPolicy>;

// Instructions for instantiating a core instance from a core module,
// corresponding to this text production:
//
//     (core instance (instantiate <modidx>) (with ...)*)`
//
struct CoreInstanceDescFromModule {
  // The core module to instantiate.
  uint32_t moduleIndex;

  // The instance's "with" declarations. In the binary format there is no inline
  // export form, only a form that uses the exports of another core instance.
  CoreInstanceInstantiateArgs args;
};

class ComponentInlineExports {
  using ExportMap = mozilla::HashMap<CacheableName, ComponentSortIndex,
                                     CacheableNameHasher, SystemAllocPolicy>;
  using OriginalIndexMap =
      mozilla::HashMap<ComponentSortIndex, uint32_t, ComponentSortIndexHasher,
                       SystemAllocPolicy>;

  // Maps from export names to ComponentSortIndexes in this "instance"'s index
  // space.
  ExportMap exports_;

  // Maps from exported items in this instance to the original indices of the
  // items being re-exported.
  OriginalIndexMap originalIndices_;

 public:
  struct Builder {
    uint32_t numFuncs = 0;
    uint32_t numTypes = 0;
    uint32_t numComponents = 0;
    uint32_t numInstances = 0;
    uint32_t numCoreFunctions = 0;
    uint32_t numCoreTables = 0;
    uint32_t numCoreMemories = 0;
    uint32_t numCoreGlobals = 0;
    uint32_t numCoreTags = 0;
    uint32_t numCoreTypes = 0;
    uint32_t numCoreModules = 0;
    uint32_t numCoreInstances = 0;

    uint32_t trackItemOfSort(ComponentSort sort);
  };

  bool addExport(Builder* builder, CacheableName&& name,
                 ComponentSortIndex exported);

  // Gets a sortidx representing the given exported item within this
  // inline-export instance (i.e. not the original sortidx of the item being
  // re-exported).
  mozilla::Maybe<ComponentSortIndex> getExport(const CacheableName& name) const;

  // Given an export that originated from this instance, resolves the sortidx of
  // the original item being re-exported. Crashes if the item does not exist.
  ComponentSortIndex resolveOriginal(ComponentSortIndex expFromThis) const;

  // Combines getExport + resolveOriginal to get the original sortidx within the
  // component, for a given export name.
  ComponentSortIndex mustResolveExportToOriginal(
      const CacheableName& name) const;
};

// Instructions for instantiating a core instance.
class CoreInstanceDesc {
  using CoreInstanceVariant =
      mozilla::Variant<CoreInstanceDescFromModule, ComponentInlineExports>;

  CoreInstanceVariant desc_;

  // The owning component for this instance.
  const Component* component_;

 public:
  explicit CoreInstanceDesc(const Component* c,
                            CoreInstanceDescFromModule&& fromModule)
      : desc_(std::move(fromModule)), component_(c) {}
  explicit CoreInstanceDesc(const Component* c,
                            ComponentInlineExports&& inlineExports)
      : desc_(std::move(inlineExports)), component_(c) {}

  const CoreInstanceVariant& desc() const { return desc_; }

  // Gets an export from a core instance by name.
  mozilla::Maybe<ComponentSortIndex> getExport(const CacheableName& name) const;

  const TypeDef& getCoreFuncType(uint32_t coreFuncIndex) const;
  const TableDesc& getTable(uint32_t tableIndex) const;
  const MemoryDesc& getMemory(uint32_t memoryIndex) const;
  const GlobalDesc& getGlobal(uint32_t globalIndex) const;
  const TagDesc& getTag(uint32_t tagIndex) const;
};

// Describes an import or export from a wasm component.
class ComponentExternDesc {
  ComponentSort sort_;
  ComponentType type_;

  // TODO(wasm-cm): This is a total hack, but since we currently don't have a
  // notion of core module types, we actually just store the index of the
  // relevant core module within the component. This obviously will not work as
  // soon as we do anything with multiple components.
  uint32_t coreModuleIndex_;

  explicit ComponentExternDesc(ComponentSort sort, ComponentType&& type)
      : sort_(sort), type_(std::move(type)) {
    MOZ_ASSERT(ComponentSortValidForExternDesc(sort));
  }
  explicit ComponentExternDesc(uint32_t coreModuleIndex)
      : sort_(ComponentSort::CoreModule), coreModuleIndex_(coreModuleIndex) {}

 public:
  ComponentExternDesc() = default;

  static ComponentExternDesc func(ComponentType&& funcType) {
    MOZ_ASSERT(funcType.kind() == ComponentTypeKind::Func);
    return ComponentExternDesc(ComponentSort::Func, std::move(funcType));
  }
  static ComponentExternDesc type(ComponentType&& type) {
    return ComponentExternDesc(ComponentSort::Type, std::move(type));
  }
  static ComponentExternDesc coreModule(uint32_t coreModuleIndex) {
    return ComponentExternDesc(coreModuleIndex);
  }

  bool isValid() const { return sort_ != ComponentSort::Invalid; }
  ComponentSort sort() const { return sort_; }
  ComponentType asFunc() const {
    MOZ_RELEASE_ASSERT(sort() == ComponentSort::Func);
    return type_;
  }
  ComponentType asType() const {
    MOZ_RELEASE_ASSERT(sort() == ComponentSort::Type);
    return type_;
  }
  uint32_t asCoreModule() const {
    MOZ_RELEASE_ASSERT(sort() == ComponentSort::CoreModule);
    // TODO(wasm-cm): This should obviously return a proper core module type,
    // when we actually support that.
    return coreModuleIndex_;
  }

  static bool matches(const ComponentExternDesc& sub,
                      const ComponentExternDesc& super);
};

static_assert(std::is_default_constructible_v<ComponentExternDesc>);

class ComponentImport {
  CacheableName name_;
  ComponentExternDesc externDesc_;

 public:
  explicit ComponentImport(CacheableName&& name,
                           const ComponentExternDesc& externDesc)
      : name_(std::move(name)), externDesc_(externDesc) {}

  const CacheableName& name() const { return name_; }
  const ComponentExternDesc& externDesc() const { return externDesc_; }
};

class ComponentExport {
  CacheableName name_;
  ComponentExternDesc externDesc_;
  // The sortidx being re-exported, e.g. for a Func-sort export, the funcs()
  // index of the func this export names.
  ComponentSortIndex exported_;

 public:
  explicit ComponentExport(CacheableName&& name, ComponentExternDesc externDesc,
                           ComponentSortIndex exported)
      : name_(std::move(name)), externDesc_(externDesc), exported_(exported) {}

  const CacheableName& name() const { return name_; }
  const ComponentExternDesc& externDesc() const { return externDesc_; }
  ComponentSortIndex exported() const { return exported_; }
};

// TODO(wasm-cm): This type is enormous, but a lot of the storage is due to
// containers like HashMap and Vector that aren't actually required once the
// component is built and validated. It would probably be smart to split this
// into ComponentBuilder and Component classes so that the final version can be
// smaller. (After all, we will have a lot of components in practice!)
class Component : public JS::WasmComponent {
 public:
  using CoreModuleVector = mozilla::Vector<SharedModule, 0, SystemAllocPolicy>;
  using CoreInstanceVector =
      mozilla::Vector<CoreInstanceDesc, 0, SystemAllocPolicy>;
  using TypeVector = mozilla::Vector<ComponentType, 0, SystemAllocPolicy>;
  using FuncVector =
      mozilla::Vector<ComponentLiftedFuncDesc, 0, SystemAllocPolicy>;
  using CoreFuncVector =
      mozilla::Vector<ComponentCoreFuncDesc, 0, SystemAllocPolicy>;
  using ImportVector = mozilla::Vector<ComponentImport, 0, SystemAllocPolicy>;
  using ExportVector = mozilla::Vector<ComponentExport, 0, SystemAllocPolicy>;
  using ItemVector = mozilla::Vector<ComponentItem, 0, SystemAllocPolicy>;
  using AliasNameMap =
      mozilla::HashMap<ComponentSortIndex, CacheableName,
                       ComponentSortIndexHasher, SystemAllocPolicy>;

 private:
  CoreModuleVector definedCoreModules_;
  // Parallel to definedCoreModules_, but populated as soon as each module's
  // slot is reserved rather than once compilation finishes (see
  // wasm::BeginCompileModule / DecodeComponent's deferred core-module compile
  // pass, which needs the whole component decoded before it can compute
  // ComponentImportHints and finish compiling each module). ModuleMetadata,
  // unlike a compiled SharedModule, has no dependency on the transient
  // bytecode buffer decoding reads from, so it's safe to keep here for the
  // type/shape queries (getCoreModuleMetadata, used by CoreInstanceDesc) that
  // decode-time validation needs before any module is fully compiled.
  mozilla::Vector<MutableModuleMetadata, 0, SystemAllocPolicy>
      definedCoreModuleMeta_;
  CoreInstanceVector definedCoreInstances_;
  TypeVector definedTypes_;
  FuncVector definedFuncs_;
  CoreFuncVector definedCoreFuncs_;
  ImportVector imports_;
  ExportVector exports_;

  ItemVector funcs_;
  ItemVector types_;
  ItemVector components_;
  ItemVector instances_;
  ItemVector coreFuncs_;
  ItemVector coreTables_;
  ItemVector coreMemories_;
  ItemVector coreGlobals_;
  ItemVector coreTags_;
  ItemVector coreTypes_;
  ItemVector coreModules_;
  ItemVector coreInstances_;

  // TODO(wasm-cm): Right now, instantiation requires us to get JS object
  // wrappers for all values we pass as imports. This means that we need to look
  // everything up from the instance's exports object, which means we need to
  // know the names of all the exports. This really should not be necessary,
  // because within the component model we have the index of everything
  // statically, but we don't store the JS objects for tables, globals, etc.
  // anywhere but the exports object today, and refactoring this is way too
  // invasive for the current state of the project. So, the current hack is to
  // just store all the names for all aliases of exports, so when we find such
  // an item, we can look it up here instead of going to the thing by index.
  AliasNameMap aliasNames_;

  // Tracks which imported resource types (by `types()` index) have already
  // had a WebIDL interface bound via tryMarkResourceInterfaceBound. A
  // Component is immutable and shared across every ComponentInstance built
  // from it (SharedComponent = RefPtr<const Component>), but binding an
  // interface mutates the resource type's shared ComponentTypeDef in place
  // rather than cloning it per instance (see setSubResourceInterface) - so a
  // second binding would retroactively change every other instantiation's
  // behavior. `mutable` because this bookkeeping happens during
  // instantiation, which is otherwise const. Not thread-safe; instantiating
  // the same Component concurrently on multiple threads is unsupported.
  mutable mozilla::HashSet<uint32_t, mozilla::DefaultHasher<uint32_t>,
                           SystemAllocPolicy>
      resourceInterfaceBoundTypeIndices_;

  template <typename T>
  bool addDefinedItem(
      ComponentSort sort, T&& item,
      mozilla::Vector<T, 0, SystemAllocPolicy>& definedItemsVector,
      ItemVector& indexSpaceVector) {
    uint32_t index = definedItemsVector.length();
    if (!definedItemsVector.append(std::forward<T>(item))) {
      return false;
    }
    return indexSpaceVector.append(ComponentItem::defined(sort, index));
  }

  // Appends `item` to the index space matching `sort`, for the sorts an
  // import or export may name (the `externdesc` sorts, minus the core-only
  // ones addDefinedItem already handles). Shared by addImport/addExport.
  [[nodiscard]] bool appendToExternIndexSpace(ComponentSort sort,
                                              ComponentItem item) {
    switch (sort) {
      case ComponentSort::Func:
        return funcs_.append(item);
      case ComponentSort::Type:
        return types_.append(item);
      case ComponentSort::Component:
        return components_.append(item);
      case ComponentSort::Instance:
        return instances_.append(item);
      case ComponentSort::CoreModule:
        return coreModules_.append(item);
      default:
        MOZ_CRASH();
    }
  }

 public:
  Component() = default;

  // --------------------------------------------------------------------------
  // Accessors and adders for each index space

  const ImportVector& imports() const { return imports_; }
  [[nodiscard]] bool addImport(ComponentImport&& import);

  const ExportVector& exports() const { return exports_; }
  [[nodiscard]] bool addExport(ComponentExport&& exp);

  const ItemVector& funcs() const { return funcs_; }
  [[nodiscard]] bool addFunc(ComponentLiftedFuncDesc&& func) {
    return addDefinedItem(ComponentSort::Func, std::move(func), definedFuncs_,
                          funcs_);
  }

  const ItemVector& types() const { return types_; }
  ComponentType getType(uint32_t typeIndex) const;
  [[nodiscard]] bool addType(ComponentType&& type) {
    MOZ_RELEASE_ASSERT(type.isValid());
    return addDefinedItem(ComponentSort::Type, std::move(type), definedTypes_,
                          types_);
  }

  // TODO(wasm-cm): Functions for components
  // TODO(wasm-cm): Functions for component instances

  const ItemVector& coreFuncs() const { return coreFuncs_; }
  [[nodiscard]] bool addAliasOfExportedCoreFunc(ComponentItem funcItem) {
    MOZ_RELEASE_ASSERT(funcItem.kind() == ComponentItem::ItemKind::Alias);
    MOZ_RELEASE_ASSERT(funcItem.sort() == ComponentSort::CoreFunction);
    return coreFuncs_.append(funcItem);
  }
  [[nodiscard]] bool addDefinedCoreFunc(ComponentCoreFuncDesc&& coreFunc) {
    uint32_t funcIndex = definedCoreFuncs_.length();
    if (!definedCoreFuncs_.append(std::move(coreFunc))) {
      return false;
    }
    return coreFuncs_.append(
        ComponentItem::defined(ComponentSort::CoreFunction, funcIndex));
  }

  const ItemVector& coreTables() const { return coreTables_; }
  const TableDesc& getCoreTable(uint32_t tableIndex) const;
  [[nodiscard]] bool addCoreTable(ComponentItem tableItem) {
    MOZ_RELEASE_ASSERT(tableItem.kind() == ComponentItem::ItemKind::Alias);
    MOZ_RELEASE_ASSERT(tableItem.sort() == ComponentSort::CoreTable);
    return coreTables_.append(tableItem);
  }

  const ItemVector& coreMemories() const { return coreMemories_; }
  const MemoryDesc& getCoreMemory(uint32_t memoryIndex) const;
  [[nodiscard]] bool addCoreMemory(ComponentItem memoryItem) {
    MOZ_RELEASE_ASSERT(memoryItem.kind() == ComponentItem::ItemKind::Alias);
    MOZ_RELEASE_ASSERT(memoryItem.sort() == ComponentSort::CoreMemory);
    return coreMemories_.append(memoryItem);
  }

  const ItemVector& coreGlobals() const { return coreGlobals_; }
  const GlobalDesc& getCoreGlobal(uint32_t globalIndex) const;
  [[nodiscard]] bool addCoreGlobal(ComponentItem globalItem) {
    MOZ_RELEASE_ASSERT(globalItem.kind() == ComponentItem::ItemKind::Alias);
    MOZ_RELEASE_ASSERT(globalItem.sort() == ComponentSort::CoreGlobal);
    return coreGlobals_.append(globalItem);
  }

  const ItemVector& coreTags() const { return coreTags_; }
  const TagDesc& getCoreTag(uint32_t tagIndex) const;
  bool addCoreTag(ComponentItem tagItem) {
    MOZ_RELEASE_ASSERT(tagItem.kind() == ComponentItem::ItemKind::Alias);
    MOZ_RELEASE_ASSERT(tagItem.sort() == ComponentSort::CoreTag);
    return coreTags_.append(tagItem);
  }

  const ItemVector& coreModules() const { return coreModules_; }
  SharedModule getCoreModule(uint32_t modIndex) const;
  // The ModuleMetadata for a core module, whether or not it has finished
  // compiling yet (see reserveCoreModule). Used for the type/shape queries
  // (CoreInstanceDesc's getExport/getCoreFuncType/getTable/getMemory/
  // getGlobal/getTag) decode-time validation needs before any module in the
  // component is fully compiled - getCoreModule() can't serve those, since
  // its SharedModule is null until setCoreModule() runs.
  const ModuleMetadata& getCoreModuleMetadata(uint32_t modIndex) const;
  // Reserves the next CoreModule index-space slot for a module whose
  // compilation hasn't finished yet (see wasm::BeginCompileModule and
  // DecodeComponent's deferred core-module compile pass, which needs the
  // whole component decoded before it can compute ComponentImportHints for
  // each module). The slot's SharedModule is null until setCoreModule()
  // backfills it.
  [[nodiscard]] bool reserveCoreModule(MutableModuleMetadata moduleMeta) {
    if (!definedCoreModuleMeta_.append(std::move(moduleMeta))) {
      return false;
    }
    return addDefinedItem(ComponentSort::CoreModule, SharedModule(nullptr),
                          definedCoreModules_, coreModules_);
  }
  // Backfills a slot reserved by reserveCoreModule(), in `definedCoreModules_`
  // index order (i.e. the order reserveCoreModule() was called).
  void setCoreModule(uint32_t definedIndex, SharedModule module) {
    MOZ_ASSERT(!definedCoreModules_[definedIndex]);
    definedCoreModules_[definedIndex] = std::move(module);
  }

  const ItemVector& coreInstances() const { return coreInstances_; }
  const CoreInstanceDesc& getCoreInstance(uint32_t instanceIndex) const;
  [[nodiscard]] bool addCoreInstance(CoreInstanceDesc&& instance) {
    return addDefinedItem(ComponentSort::CoreInstance, std::move(instance),
                          definedCoreInstances_, coreInstances_);
  }

  // Given a `sortidx` or `core:sortidx`, resolve the actual component item from
  // the appropriate index space.
  ComponentItem resolveSortIndex(ComponentSortIndex sortIndex) const;

  [[nodiscard]] bool saveExportNameForAlias(ComponentSortIndex sortIndexOfAlias,
                                            CacheableName&& name);
  const CacheableName& getExportNameForAlias(
      ComponentSortIndex sortIndexOfAlias) const;

  // --------------------------------------------------------------------------
  // Utilities for accessing type information

  // Gets the type of a component func (not a core func). It is always safe to
  // call `.asFunc()` on the result.
  ComponentType getTypeForFunc(uint32_t funcIndex) const;

  // Gets the type of a core func (not a component func).
  const TypeDef& getTypeForCoreFunc(uint32_t coreFuncIndex) const;

  // Gets the `canon lift` descriptor for a `funcs()` index. Only valid to
  // call when that index is a `Defined` item.
  const ComponentLiftedFuncDesc& getLiftedFunc(uint32_t funcIndex) const;

  // Gets the `canon lower`/builtin descriptor for a `coreFuncs()` index. Only
  // valid to call when that index is a `Defined` item.
  const ComponentCoreFuncDesc& getLoweredCoreFunc(uint32_t coreFuncIndex) const;

  size_t gcMallocBytesExcludingCode() const {
    // TODO(wasm-cm): Right now, this only sums up the sizes of the inner
    // modules, but this is not an accurate picture of a component's memory
    // footprint.
    size_t total = 0;
    for (const SharedModule& module : definedCoreModules_) {
      total += module->gcMallocBytesExcludingCode();
    }
    return total;
  }

  size_t tier1CodeMemoryUsed() const {
    // TODO(wasm-cm): As above, this only sums up the memory for core modules,
    // and does not account for other potential code memory.
    size_t total = 0;
    for (const SharedModule& module : definedCoreModules_) {
      total += module->tier1CodeMemoryUsed();
    }
    return total;
  }

  // --------------------------------------------------------------------------
  // Instantiation

  [[nodiscard]] bool instantiate(
      JSContext* cx, HandleObject instanceProto, HandleObject importObj,
      MutableHandle<WasmComponentInstanceObject*> instance) const;

  // Records that a WebIDL interface is being bound to the imported resource
  // type at `typeIndex`. Returns false only on OOM; `*alreadyBound` reports
  // whether an earlier instantiation already bound one (see
  // resourceInterfaceBoundTypeIndices_) - the caller should treat that as a
  // real error, not proceed to call setSubResourceInterface again.
  [[nodiscard]] bool tryMarkResourceInterfaceBound(uint32_t typeIndex,
                                                   bool* alreadyBound) const;

 private:
  // JS API and JS::WasmComponent implementation:
  JSObject* createObject(JSContext* cx) const override;
};

using MutableComponent = RefPtr<Component>;
using SharedComponent = RefPtr<const Component>;

// The System-ABI argument shape of a JS::WebIDLNativeCall: (adapter, ctx,
// params, resultMemBase, results). Shared by every place that builds a call
// to a `LoweredNativeClosure::entry` directly - GenerateCanonLowerNative's
// per-instantiation stub, BaseCompiler::emitCallComponentCanonLower's
// call-site inlining, and FunctionCompiler::callComponentCanonLower's - so
// they all agree on the StackArgAreaSizeUnaligned this call needs.
class NativeCallArgTypes {
  static constexpr jit::MIRType kTypes[] = {
      jit::MIRType::Pointer,  // adapter
      jit::MIRType::Pointer,  // ctx
      jit::MIRType::Pointer,  // params
      jit::MIRType::Int32,    // resultMemBase
      jit::MIRType::Pointer,  // results
  };

 public:
  size_t length() const { return sizeof(kTypes) / sizeof(kTypes[0]); }
  jit::MIRType operator[](size_t i) const { return kTypes[i]; }
};

class ComponentInstance;

// All the state a `canon lower` core func needs to call straight into a
// `JS::WebIDLNativeCall` entry, built once at instantiation time (see
// ComponentInstance::init) instead of per call - either a native overload
// selected for the callee (see SelectNativeOverload), or the generic entry
// (see LoweredNativeCall in WasmComponent.cpp) that drives an arbitrary JS
// callee through the same adapter machinery. Owned by ComponentInstance via
// a UniquePtr, so its address is stable even if the owning vector is ever
// reallocated - a JIT-generated stub (GenerateCanonLowerNative) bakes a raw
// pointer to it directly into compiled code.
struct LoweredNativeClosure {
  JS::WebIDLNativeCall entry = nullptr;
  CallAdapter adapter;
  JS::WebIDLContext ctx;

  // Scratch storage reused across every call through this closure: sized
  // exactly to this overload's flat param/result budget and filled in
  // immediately before, and drained immediately after, each native call, so
  // reuse (including across a same-thread reentrant call) is safe. Contains
  // no GC pointers, so needs no tracing. Allocated once, up front, and never
  // resized again afterward: JIT-generated code reads `paramsScratch.get()` /
  // `resultsScratch.get()`'s effect directly - either baked as an immediate
  // (a stub compiled with `this` already known, see GenerateCanonLowerNative)
  // or loaded at runtime through `this` (a core module inlining the call, see
  // ComponentImportKind::CanonLower) - so these must be plain owned arrays,
  // not a Vector: a JIT-generated load can only read a raw pointer field, not
  // replicate Vector's internal layout.
  mozilla::UniquePtr<JS::WasmScalarVal[]> paramsScratch;
  mozilla::UniquePtr<JS::WasmScalarVal[]> resultsScratch;
  uint32_t numParams = 0;
  uint32_t numResultSlots = 0;

  // Which wasm param (if any) carries the linear-memory offset a spilled
  // result must be written at, per the canonical ABI's caller-allocated
  // out-param convention. -1 if this overload's result fits flat.
  int32_t resultOutParamIndex = -1;

  // Only read by the generic entry (LoweredNativeCall): the owning instance
  // and the lowered func's index into its component func space, needed to
  // recover the actual callee/name-kind/func-type a native overload instead
  // gets to bake into `entry`/`adapter` ahead of time. `instance` is a
  // back-pointer, not an owning edge - this closure is itself owned
  // (transitively, via ComponentInstance::loweredNativeClosures_) by the
  // instance it points to, so it cannot outlive it.
  ComponentInstance* instance = nullptr;
  uint32_t loweredFuncIndex = 0;

  // Recovers the owning closure from the `ctx` pointer a JS::WebIDLNativeCall
  // is called with - the ABI has no room for a callee/instance argument, so
  // the generic entry (the only one that needs this) gets back to its own
  // closure this way instead.
  static LoweredNativeClosure* fromCtx(JS::WebIDLContext* ctx) {
    return reinterpret_cast<LoweredNativeClosure*>(
        reinterpret_cast<uint8_t*>(ctx) - offsetof(LoweredNativeClosure, ctx));
  }

  void trace(JSTracer* trc) { ctx.trace(trc); }
};

static_assert(sizeof(mozilla::UniquePtr<JS::WasmScalarVal[]>) ==
                  sizeof(JS::WasmScalarVal*),
              "for JIT access");

class ComponentInstance {
  // The containing JS::Realm.
  JS::Realm* realm_;

  // The containing JSContext.
  JSContext* cx_;

  // The wasm::Component for this instance.
  const SharedComponent component_;

  // The JS object that owns this ComponentInstance - a back-pointer only, so
  // weak. Lift/lower closures hold their own strong reference to it.
  WeakHeapPtr<WasmComponentInstanceObject*> object_;

  using CoreInstanceVector =
      GCVector<HeapPtr<WasmInstanceObject*>, 0, CellAllocPolicy>;
  // An array of all the core instances owned by this component instance. NOTE!
  // This array is sparse; its indices will always correspond 1:1 with
  // Component::coreInstances(), but not all such instances will get a
  // WasmInstanceObject.
  CoreInstanceVector coreInstances_;

  using ComponentFuncVector = GCVector<HeapPtr<JSObject*>, 0, CellAllocPolicy>;
  // The component function index space (1:1 with Component::funcs()): an
  // imported func resolves to whatever JS callable was passed in for it; a
  // `canon lift`ed func resolves to its lift closure. Populated in full
  // during `init`, before any core module is instantiated.
  ComponentFuncVector componentFuncs_;

  // 1:1 with componentFuncs_: the bracket tag the func was imported under,
  // which decides how `canon lower` invokes it. `[method]`'s first flat param
  // is a `borrow<self>` to bind as the callee's `this` rather than pass
  // positionally; `[constructor]` must be invoked as a construct, not a call.
  // `[static]` and an untagged name both behave as Plain here.
  mozilla::Vector<ComponentNameKind, 0, SystemAllocPolicy>
      componentFuncNameKinds_;

  using LoweredNativeClosureVector =
      mozilla::Vector<mozilla::UniquePtr<LoweredNativeClosure>, 0,
                      SystemAllocPolicy>;
  // Sparse, 1:1 with Component::coreFuncs(): non-null for every `canon
  // lower` core func (every one gets a closure, whether or not its callee
  // has a native overload - see ResolveLoweredCoreFunc), null for every
  // other kind of core func. Resized once, up front, in `init`.
  LoweredNativeClosureVector loweredNativeClosures_;

  // The resource handle table for this instance, shared by every `canon
  // lift`/`canon lower` closure it owns.
  ComponentHandleTable handles_;

  // Only WasmComponentInstanceObject can call the private trace function.
  friend class js::WasmComponentInstanceObject;
  void tracePrivate(JSTracer* trc);

  [[nodiscard]] bool resolveCoreExport(ComponentSort sort, uint32_t index,
                                       JS::MutableHandleValue out) const;

 public:
  ComponentInstance(JSContext* cx, Handle<WasmComponentInstanceObject*> object,
                    const SharedComponent component);
  ~ComponentInstance();

  static ComponentInstance* create(JSContext* cx,
                                   Handle<WasmComponentInstanceObject*> object,
                                   const SharedComponent component);
  static void destroy(ComponentInstance* instance);

  [[nodiscard]] bool init(JSContext* cx, HandleObject importObj);

  JSContext* cx() const { return cx_; }
  const SharedComponent& component() const { return component_; }
  WasmComponentInstanceObject* object() const { return object_; }

  // Gets a core instance object for a given index in the component, if one
  // exists. (It may not; inline-export instances do not get actual objects.)
  // The result may therefore be null.
  WasmInstanceObject* coreInstance(uint32_t index) const {
    if (coreInstances_.length() <= index) {
      return nullptr;
    }
    return coreInstances_[index];
  }

  [[nodiscard]] bool resizeComponentFuncs(uint32_t length) {
    return componentFuncs_.resize(length) &&
           componentFuncNameKinds_.resize(length);
  }
  void setComponentFunc(uint32_t funcIndex, JSObject* value) {
    componentFuncs_[funcIndex] = value;
  }
  JSObject* componentFunc(uint32_t funcIndex) const {
    return componentFuncs_[funcIndex];
  }
  void setComponentFuncNameKind(uint32_t funcIndex, ComponentNameKind kind) {
    componentFuncNameKinds_[funcIndex] = kind;
  }
  ComponentNameKind componentFuncNameKind(uint32_t funcIndex) const {
    return componentFuncNameKinds_[funcIndex];
  }

  [[nodiscard]] bool resizeLoweredNativeClosures(uint32_t length) {
    return loweredNativeClosures_.resize(length);
  }
  void setLoweredNativeClosure(
      uint32_t coreFuncIndex,
      mozilla::UniquePtr<LoweredNativeClosure> closure) {
    loweredNativeClosures_[coreFuncIndex] = std::move(closure);
  }
  LoweredNativeClosure* loweredNativeClosure(uint32_t coreFuncIndex) const {
    return loweredNativeClosures_[coreFuncIndex].get();
  }

  ComponentHandleTable& handles() { return handles_; }
  static constexpr size_t offsetOfHandles() {
    return offsetof(ComponentInstance, handles_);
  }

  // Resolves a `coreMemories()`/`coreFuncs()` index (as referenced by a
  // ComponentCanonOpts memory/realloc/postReturn index) to the runtime JS
  // value of the already-instantiated core instance export it aliases.
  [[nodiscard]] bool resolveCoreMemory(
      uint32_t memIndex, JS::MutableHandle<WasmMemoryObject*> out) const;
  [[nodiscard]] bool resolveCoreFunc(uint32_t coreFuncIndex,
                                     JS::MutableHandleObject out) const;
};

UniqueChars ToString(ComponentItem item);
UniqueChars ToString(ComponentSortIndex sortIndex);

}  // namespace wasm
}  // namespace js

#endif  // ENABLE_WASM_COMPONENTS

#endif  // wasm_component_h
