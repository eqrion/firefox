/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "wasm/WasmComponent.h"
#include "mozilla/EndianUtils.h"
#include "mozilla/Maybe.h"
#include "wasm/WasmBinary.h"
#include "wasm/WasmCompileArgs.h"
#include "wasm/WasmFeatures.h"
#include "wasm/WasmGenerator.h"
#include "wasm/WasmInstance.h"
#include "wasm/WasmMetadata.h"
#include "wasm/WasmModuleTypes.h"
#include "wasm/WasmShareable.h"
#include "wasm/WasmValidate.h"

#ifdef ENABLE_WASM_COMPONENTS

#  include "mozilla/Casting.h"    // mozilla::BitwiseCast
#  include "mozilla/TextUtils.h"  // mozilla::IsAscii
#  include "mozilla/Utf8.h"       // mozilla::ConvertUtf16toUtf8{,Partial}

#  include <algorithm>  // std::max
#  include <cstring>    // std::strlen

#  include "jsapi.h"  // JS_New*String*, JS_EncodeStringToUTF8
#  include "jsfriendapi.h"  // js::NewFunctionWithReserved, js::SetFunctionNativeReserved

#  include "js/Array.h"                 // JS::NewArrayObject
#  include "js/ArrayBuffer.h"           // JS::NewArrayBuffer, JS::ArrayBuffer
#  include "js/BigInt.h"                // JS::BigInt::createFromInt64/Uint64
#  include "js/CallAndConstruct.h"      // JS::Call
#  include "js/CharacterEncoding.h"     // JS::UTF8Chars
#  include "js/Conversions.h"           // JS::ToUint32
#  include "js/experimental/JitInfo.h"  // JSJitInfo, JSWebIDLMethodJitInfo
#  include "js/experimental/TypedData.h"  // JS_NewUint8{,Clamped}Array, JS::Uint8{,Clamped}Array
#  include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#  include "js/Printer.h"               // js::QuoteString
#  include "js/Printf.h"                // JS_smprintf
#  include "js/PropertyAndElement.h"    // JS_NewPlainObject, JS_DefineProperty
#  include "js/String.h"                // JS_NewUCStringCopyN
#  include "js/ValueArray.h"            // JS::RootedValueArray
#  include "js/Wrapper.h"               // js::UncheckedUnwrap
#  include "threading/ExclusiveData.h"
#  include "util/Text.h"
#  include "vm/GlobalObject.h"
#  include "vm/Interpreter.h"  // js::FixedInvokeArgs
#  include "vm/JSFunction.h"   // JSFunction::hasJitInfo/jitInfo
#  include "vm/JSObject.h"
#  include "vm/Logging.h"  // JS_LOG
#  include "vm/MutexIDs.h"
#  include "vm/ObjectOperations.h"  // js::PreventExtensions
#  include "vm/PlainObject.h"       // js::NewPlainObjectWithProto
#  include "vm/StringType.h"        // JSString, JSLinearString
#  include "wasm/WasmJS.h"

#  include "vm/ArrayBufferObject-inl.h"  // ArrayBufferObjectMaybeShared::dataPointerEither
#  include "vm/JSAtomUtils-inl.h"
#  include "vm/Realm-inl.h"  // js::AutoRealmUnchecked
#  include "wasm/WasmInstance-inl.h"

using namespace js;
using namespace js::wasm;
using JS::WebIDLContext;

static constexpr mozilla::Span<const char> attributeConstructor =
    mozilla::MakeStringSpan("[constructor]");
static constexpr mozilla::Span<const char> attributeMethod =
    mozilla::MakeStringSpan("[method]");
static constexpr mozilla::Span<const char> attributeStatic =
    mozilla::MakeStringSpan("[static]");

// Component model names are encoded as UTF-8, and in fact an ASCII subset of
// UTF-8, so this is fine.
static char LowercaseNameChar(char c) {
  return ('A' <= c && c <= 'Z') ? c + ('a' - 'A') : c;
}

// "element-as-node" -> "elementAsNode". Component model labels are the ASCII
// subset of UTF-8 mentioned above, so this is a plain byte-level transform.
[[nodiscard]] static bool KebabToCamel(JSContext* cx,
                                       mozilla::Span<const char> label,
                                       UTF8Bytes* camel) {
  size_t camelLen = 0;
  for (char c : label) {
    if (c != '-') {
      camelLen++;
    }
  }
  if (!camel->resizeUninitialized(camelLen)) {
    ReportOutOfMemory(cx);
    return false;
  }
  size_t i = 0;
  bool uppercaseNext = false;
  for (char c : label) {
    if (c == '-') {
      uppercaseNext = true;
      continue;
    }
    if (uppercaseNext && 'a' <= c && c <= 'z') {
      c = char(c - ('a' - 'A'));
    }
    uppercaseNext = false;
    (*camel)[i++] = c;
  }
  return true;
}

static JSAtom* KebabToCamelAtom(JSContext* cx,
                                mozilla::Span<const char> label) {
  UTF8Bytes camel;
  if (!KebabToCamel(cx, label, &camel)) {
    return nullptr;
  }
  return AtomizeUTF8Chars(cx, camel.begin(), camel.length());
}

// "http-headers" -> "HttpHeaders". Resource type names bind to a JS class
// rather than a plain function/field, so unlike KebabToCamel, the first
// letter is also uppercased - matching the PascalCase convention JS/WebIDL
// interface and class names use (and that component tooling such as jco
// uses for WIT resources).
[[nodiscard]] static bool KebabToPascal(JSContext* cx,
                                        mozilla::Span<const char> label,
                                        UTF8Bytes* pascal) {
  if (!KebabToCamel(cx, label, pascal)) {
    return false;
  }
  if (pascal->length() != 0 && 'a' <= (*pascal)[0] && (*pascal)[0] <= 'z') {
    (*pascal)[0] = char((*pascal)[0] - ('a' - 'A'));
  }
  return true;
}

static JSAtom* KebabToPascalAtom(JSContext* cx,
                                 mozilla::Span<const char> label) {
  UTF8Bytes pascal;
  if (!KebabToPascal(cx, label, &pascal)) {
    return nullptr;
  }
  return AtomizeUTF8Chars(cx, pascal.begin(), pascal.length());
}

static mozilla::Span<const char> TrimAttribute(mozilla::Span<const char> name) {
  if (CharsStartsWith(name, attributeConstructor)) {
    return name.Subspan(attributeConstructor.Length());
  }
  if (CharsStartsWith(name, attributeMethod)) {
    return name.Subspan(attributeMethod.Length());
  }
  if (CharsStartsWith(name, attributeStatic)) {
    return name.Subspan(attributeStatic.Length());
  }
  return name;
}

static bool NameHasAttribute(mozilla::Span<const char> name) {
  // The name should already be well-formed from parse time.
  return name.Length() == 0 || name.data()[0] == '[';
}

// Splits a raw component name into its bracket-tag classification, for
// resolving `[constructor]`/`[method]`/`[static]` imports against a
// resource's JS constructor (Web.md "Read the imports object"). For
// `[method]r.m`/`[static]r.m`, `resource` is `r` and `member` is `m`; for
// `[constructor]r`, `resource` is `r` and `member` is empty; for a plain
// name, `member` is the whole name and `resource` is empty.
struct ParsedComponentName {
  ComponentNameKind kind;
  mozilla::Span<const char> resource;
  mozilla::Span<const char> member;
};

static ParsedComponentName ParseComponentName(mozilla::Span<const char> name) {
  if (CharsStartsWith(name, attributeConstructor)) {
    return {ComponentNameKind::Constructor,
            name.Subspan(attributeConstructor.Length()),
            {}};
  }
  bool isMethod = CharsStartsWith(name, attributeMethod);
  bool isStatic = !isMethod && CharsStartsWith(name, attributeStatic);
  if (isMethod || isStatic) {
    mozilla::Span<const char> tagged = name.Subspan(
        isMethod ? attributeMethod.Length() : attributeStatic.Length());
    size_t dot = tagged.IndexOf('.');
    MOZ_RELEASE_ASSERT(dot != mozilla::Span<const char>::npos);
    auto [resource, dottedMember] = tagged.SplitAt(dot);
    return {isMethod ? ComponentNameKind::Method : ComponentNameKind::Static,
            resource, dottedMember.Subspan(1)};
  }
  return {ComponentNameKind::Plain, {}, name};
}

// We hash only the base part of the name, e.g. "foo" for "[constructor]foo".
HashNumber StronglyUniqueNameHasher::hash(const Lookup& aLookup) {
  mozilla::Span<const char> trimmed = TrimAttribute(aLookup);

  HashNumber hash = 0;
  for (size_t i = 0; i < trimmed.Length(); i++) {
    char c = trimmed.data()[i];
    if (c == '.') {
      break;
    }
    hash = mozilla::AddToHash(hash, LowercaseNameChar(trimmed.data()[i]));
  }
  return hash;
}

bool StronglyUniqueNameHasher::match(const Key& aKey, const Lookup& aLookup) {
  mozilla::Span<const char> keyBytes = aKey.utf8Bytes();
  mozilla::Span<const char> newTrimmed = TrimAttribute(aLookup);
  mozilla::Span<const char> existingTrimmed = TrimAttribute(keyBytes);

  // Rule 1: If one name is l and the other name is [constructor]l (for the
  // same label l), they are strongly-unique.
  bool newIsConstructor = CharsStartsWith(aLookup, attributeConstructor);
  bool existingIsConstructor = CharsStartsWith(keyBytes, attributeConstructor);
  if (newIsConstructor != existingIsConstructor &&
      newTrimmed == existingTrimmed) {
    return false;
  }

  // Rule 2: If one name is l and the other name is [*]l.l (for the same label l
  // and any annotation * with a dotted l.l name), they are not strongly-unique.
  mozilla::Maybe<mozilla::Span<const char>> plain;
  mozilla::Maybe<mozilla::Span<const char>> dotted;
  if (!NameHasAttribute(aLookup)) {
    plain.emplace(aLookup);
  } else if (!NameHasAttribute(keyBytes)) {
    plain.emplace(keyBytes);
  }
  if (CharsStartsWith(aLookup, attributeMethod) ||
      CharsStartsWith(aLookup, attributeStatic)) {
    dotted.emplace(aLookup);
  } else if (CharsStartsWith(keyBytes, attributeMethod) ||
             CharsStartsWith(keyBytes, attributeStatic)) {
    dotted.emplace(keyBytes);
  }
  if (plain.isSome() && dotted.isSome()) {
    mozilla::Span<const char> dottedTrimmed = TrimAttribute(dotted.value());
    size_t indexOfDot = dottedTrimmed.IndexOf('.');
    MOZ_RELEASE_ASSERT(indexOfDot != mozilla::Span<const char>::npos);
    auto [before, after] = dottedTrimmed.SplitAt(indexOfDot);
    after = after.Subspan(1);  // The SplitAt method includes the dot.
    if (plain.value() == after && plain.value() == before) {
      return true;
    }
  }

  // Rule 3: Lowercase the names, trim attributes, and compare directly.
  if (newTrimmed.Length() != existingTrimmed.Length()) {
    return false;
  }
  for (size_t i = 0; i < newTrimmed.Length(); i++) {
    if (LowercaseNameChar(newTrimmed[i]) !=
        LowercaseNameChar(existingTrimmed[i])) {
      return false;
    }
  }
  return true;
}

bool StronglyUniqueNameSet::add(mozilla::Span<const char> name,
                                bool* duplicate) {
  *duplicate = false;

  auto p = data_.lookupForAdd(name);
  if (p) {
    *duplicate = true;
    return true;
  }

  CacheableName owned;
  if (!CacheableName::fromUTF8Bytes(name, &owned)) {
    return false;
  }
  return data_.add(p, std::move(owned));
}

uint32_t ComponentInlineExports::Builder::trackItemOfSort(ComponentSort sort) {
  switch (sort) {
    case ComponentSort::Func:
      return numFuncs++;
    case ComponentSort::Type:
      return numTypes++;
    case ComponentSort::Component:
      return numComponents++;
    case ComponentSort::Instance:
      return numInstances++;
    case ComponentSort::CoreFunction:
      return numCoreFunctions++;
    case ComponentSort::CoreTable:
      return numCoreTables++;
    case ComponentSort::CoreMemory:
      return numCoreMemories++;
    case ComponentSort::CoreGlobal:
      return numCoreGlobals++;
    case ComponentSort::CoreTag:
      return numCoreTags++;
    case ComponentSort::CoreType:
      return numCoreTypes++;
    case ComponentSort::CoreModule:
      return numCoreModules++;
    case ComponentSort::CoreInstance:
      return numCoreInstances++;
    default:
      MOZ_CRASH();
  }
}

bool ComponentInlineExports::addExport(Builder* builder, CacheableName&& name,
                                       ComponentSortIndex exported) {
  uint32_t indexInThisInstance = builder->trackItemOfSort(exported.sort);
  ComponentSortIndex itemInThisInstance(exported.sort, indexInThisInstance);

  // Add export name -> item-in-this-instance mapping
  auto p1 = exports_.lookupForAdd(name.utf8Bytes());
  MOZ_RELEASE_ASSERT(!p1, "duplicates should have been caught in validation");
  if (!exports_.add(p1, std::move(name), itemInThisInstance)) {
    return false;
  }

  // Add item-in-this-instance -> original index mapping
  auto p2 = originalIndices_.lookupForAdd(itemInThisInstance);
  MOZ_RELEASE_ASSERT(!p2);
  if (!originalIndices_.add(p2, itemInThisInstance, exported.index)) {
    return false;
  }

  return true;
}

mozilla::Maybe<ComponentSortIndex> ComponentInlineExports::getExport(
    const CacheableName& name) const {
  auto p = exports_.lookup(name.utf8Bytes());
  if (!p) {
    return mozilla::Nothing();
  }
  return mozilla::Some(p->value());
}

ComponentSortIndex ComponentInlineExports::resolveOriginal(
    ComponentSortIndex expFromThis) const {
  auto p = originalIndices_.lookup(expFromThis);
  MOZ_RELEASE_ASSERT(p.found());
  ComponentSortIndex orig = ComponentSortIndex(expFromThis.sort, p->value());
  MOZ_ASSERT(orig.sort == expFromThis.sort);
  return orig;
}

ComponentSortIndex ComponentInlineExports::mustResolveExportToOriginal(
    const CacheableName& name) const {
  ComponentSortIndex expInThis = getExport(name).value();
  return resolveOriginal(expInThis);
}

mozilla::Maybe<ComponentSortIndex> CoreInstanceDesc::getExport(
    const CacheableName& name) const {
  return desc_.match(
      [&](const CoreInstanceDescFromModule& fromModule)
          -> mozilla::Maybe<ComponentSortIndex> {
        const ModuleMetadata& modMeta =
            component_->getCoreModuleMetadata(fromModule.moduleIndex);
        mozilla::Maybe<const Export&> exp = modMeta.getExport(name);
        if (exp.isNothing()) {
          return mozilla::Nothing();
        }
        switch (exp->kind()) {
          case DefinitionKind::Function:
            return mozilla::Some(ComponentSortIndex(ComponentSort::CoreFunction,
                                                    exp->funcIndex()));
          case DefinitionKind::Table:
            return mozilla::Some(ComponentSortIndex(ComponentSort::CoreTable,
                                                    exp->tableIndex()));
          case DefinitionKind::Memory:
            return mozilla::Some(ComponentSortIndex(ComponentSort::CoreMemory,
                                                    exp->memoryIndex()));
          case DefinitionKind::Global:
            return mozilla::Some(ComponentSortIndex(ComponentSort::CoreGlobal,
                                                    exp->globalIndex()));
          case DefinitionKind::Tag:
            return mozilla::Some(
                ComponentSortIndex(ComponentSort::CoreTag, exp->tagIndex()));
          default:
            MOZ_CRASH();
        }
      },
      [&](const ComponentInlineExports& inlineExports)
          -> mozilla::Maybe<ComponentSortIndex> {
        return inlineExports.getExport(name);
      });
}

const TypeDef& CoreInstanceDesc::getCoreFuncType(uint32_t coreFuncIndex) const {
  return desc_.match(
      [&](const CoreInstanceDescFromModule& fromModule) -> const TypeDef& {
        const ModuleMetadata& modMeta =
            component_->getCoreModuleMetadata(fromModule.moduleIndex);
        return modMeta.codeMeta->getFuncTypeDef(coreFuncIndex);
      },
      [&](const ComponentInlineExports& inlineExports) -> const TypeDef& {
        ComponentSortIndex originalFunc = inlineExports.resolveOriginal(
            ComponentSortIndex(ComponentSort::CoreFunction, coreFuncIndex));
        return component_->getTypeForCoreFunc(originalFunc.index);
      });
}

const TableDesc& CoreInstanceDesc::getTable(uint32_t tableIndex) const {
  return desc_.match(
      [&](const CoreInstanceDescFromModule& fromModule) -> const TableDesc& {
        const ModuleMetadata& modMeta =
            component_->getCoreModuleMetadata(fromModule.moduleIndex);
        return modMeta.codeMeta->tables[tableIndex];
      },
      [&](const ComponentInlineExports& inlineExports) -> const TableDesc& {
        ComponentSortIndex originalTable = inlineExports.resolveOriginal(
            ComponentSortIndex(ComponentSort::CoreTable, tableIndex));
        return component_->getCoreTable(originalTable.index);
      });
}

const MemoryDesc& CoreInstanceDesc::getMemory(uint32_t memoryIndex) const {
  return desc_.match(
      [&](const CoreInstanceDescFromModule& fromModule) -> const MemoryDesc& {
        const ModuleMetadata& modMeta =
            component_->getCoreModuleMetadata(fromModule.moduleIndex);
        return modMeta.codeMeta->memories[memoryIndex];
      },
      [&](const ComponentInlineExports& inlineExports) -> const MemoryDesc& {
        ComponentSortIndex originalMemory = inlineExports.resolveOriginal(
            ComponentSortIndex(ComponentSort::CoreMemory, memoryIndex));
        return component_->getCoreMemory(originalMemory.index);
      });
}

const GlobalDesc& CoreInstanceDesc::getGlobal(uint32_t globalIndex) const {
  return desc_.match(
      [&](const CoreInstanceDescFromModule& fromModule) -> const GlobalDesc& {
        const ModuleMetadata& modMeta =
            component_->getCoreModuleMetadata(fromModule.moduleIndex);
        return modMeta.codeMeta->globals[globalIndex];
      },
      [&](const ComponentInlineExports& inlineExports) -> const GlobalDesc& {
        ComponentSortIndex originalGlobal = inlineExports.resolveOriginal(
            ComponentSortIndex(ComponentSort::CoreGlobal, globalIndex));
        return component_->getCoreGlobal(originalGlobal.index);
      });
}

const TagDesc& CoreInstanceDesc::getTag(uint32_t tagIndex) const {
  return desc_.match(
      [&](const CoreInstanceDescFromModule& fromModule) -> const TagDesc& {
        const ModuleMetadata& modMeta =
            component_->getCoreModuleMetadata(fromModule.moduleIndex);
        return modMeta.codeMeta->tags[tagIndex];
      },
      [&](const ComponentInlineExports& inlineExports) -> const TagDesc& {
        ComponentSortIndex originalTag = inlineExports.resolveOriginal(
            ComponentSortIndex(ComponentSort::CoreTag, tagIndex));
        return component_->getCoreTag(originalTag.index);
      });
}

bool ComponentExternDesc::matches(const ComponentExternDesc& sub,
                                  const ComponentExternDesc& super) {
  MOZ_ASSERT(ComponentSortValidForExternDesc(sub.sort()));
  MOZ_ASSERT(ComponentSortValidForExternDesc(super.sort()));
  MOZ_RELEASE_ASSERT(sub.isValid() && super.isValid());

  // Different sorts never match.
  if (sub.sort() != super.sort()) {
    return false;
  }

  switch (sub.sort()) {
    case ComponentSort::Func:
      return sub.asFunc() == super.asFunc();
    case ComponentSort::Type:
      return sub.asType() == super.asType();
    case ComponentSort::Component:
    case ComponentSort::Instance:
    case ComponentSort::CoreModule: {
      // TODO(wasm-cm)
      return false;
    } break;
    default:
      MOZ_CRASH("all valid sorts for externdesc should have been handled");
  }
}

bool ComponentType::record(ComponentRecordFieldVector&& fields,
                           ComponentType* type) {
  ComponentTypeDef* def =
      js_new<ComponentTypeDef>(ComponentTypeSchema(std::move(fields)));
  if (!def) {
    return false;
  }
  *type = ComponentType(ComponentTypeKind::Record, def);
  return true;
}

bool ComponentType::variant(ComponentVariantCaseVector&& cases,
                            ComponentType* type) {
  ComponentTypeDef* def =
      js_new<ComponentTypeDef>(ComponentTypeSchema(std::move(cases)));
  if (!def) {
    return false;
  }
  *type = ComponentType(ComponentTypeKind::Variant, def);
  return true;
}

bool ComponentType::list(ComponentType&& elemType, ComponentType* type) {
  ComponentTypeDef* def =
      js_new<ComponentTypeDef>(ComponentTypeSchema(std::move(elemType)));
  if (!def) {
    return false;
  }
  *type = ComponentType(ComponentTypeKind::List, def);
  return true;
}

bool ComponentType::tuple(ComponentTypeVector&& items, ComponentType* type) {
  ComponentTypeDef* def =
      js_new<ComponentTypeDef>(ComponentTypeSchema(std::move(items)));
  if (!def) {
    return false;
  }
  *type = ComponentType(ComponentTypeKind::Tuple, def);
  return true;
}

bool ComponentType::flags(CacheableNameVector&& labels, ComponentType* type) {
  ComponentTypeDef* def =
      js_new<ComponentTypeDef>(ComponentTypeSchema(std::move(labels)));
  if (!def) {
    return false;
  }
  *type = ComponentType(ComponentTypeKind::Flags, def);
  return true;
}

bool ComponentType::enum_(CacheableNameVector&& cases, ComponentType* type) {
  ComponentTypeDef* def =
      js_new<ComponentTypeDef>(ComponentTypeSchema(std::move(cases)));
  if (!def) {
    return false;
  }
  *type = ComponentType(ComponentTypeKind::Enum, def);
  return true;
}

bool ComponentType::option(ComponentType&& inner, ComponentType* type) {
  ComponentTypeDef* def =
      js_new<ComponentTypeDef>(ComponentTypeSchema(std::move(inner)));
  if (!def) {
    return false;
  }
  *type = ComponentType(ComponentTypeKind::Option, def);
  return true;
}

bool ComponentType::result(ComponentResultType&& inner, ComponentType* type) {
  ComponentTypeDef* def =
      js_new<ComponentTypeDef>(ComponentTypeSchema(std::move(inner)));
  if (!def) {
    return false;
  }
  *type = ComponentType(ComponentTypeKind::Result, def);
  return true;
}

bool ComponentType::own(ComponentType&& inner, ComponentType* type) {
  ComponentTypeDef* def =
      js_new<ComponentTypeDef>(ComponentTypeSchema(std::move(inner)));
  if (!def) {
    return false;
  }
  *type = ComponentType(ComponentTypeKind::Own, def);
  return true;
}

bool ComponentType::borrow(ComponentType&& inner, ComponentType* type) {
  ComponentTypeDef* def =
      js_new<ComponentTypeDef>(ComponentTypeSchema(std::move(inner)));
  if (!def) {
    return false;
  }
  *type = ComponentType(ComponentTypeKind::Borrow, def);
  return true;
}

bool ComponentType::func(ComponentFuncType&& inner, ComponentType* type) {
  ComponentTypeDef* def =
      js_new<ComponentTypeDef>(ComponentTypeSchema(std::move(inner)));
  if (!def) {
    return false;
  }
  *type = ComponentType(ComponentTypeKind::Func, def);
  return true;
}

bool ComponentType::resource(ComponentResourceType&& inner,
                             ComponentType* type) {
  ComponentTypeDef* def =
      js_new<ComponentTypeDef>(ComponentTypeSchema(std::move(inner)));
  if (!def) {
    return false;
  }
  *type = ComponentType(ComponentTypeKind::Resource, def);
  return true;
}

bool ComponentType::subResource(ComponentType* type) {
  // We still need a unique heap allocation so that two (sub resource) types
  // will not be equal. The ComponentResourceType payload starts with no
  // interface; setSubResourceInterface binds one later, once instantiation
  // resolves the import.
  ComponentTypeDef* def =
      js_new<ComponentTypeDef>(ComponentTypeSchema(ComponentResourceType()));
  if (!def) {
    return false;
  }
  *type = ComponentType(ComponentTypeKind::SubResource, def);
  return true;
}

const ComponentRecordFieldVector& ComponentType::asRecord() const {
  MOZ_RELEASE_ASSERT(kind() == ComponentTypeKind::Record);
  return typeDef_->schema().as<ComponentRecordFieldVector>();
}

const ComponentVariantCaseVector& ComponentType::asVariant() const {
  MOZ_RELEASE_ASSERT(kind() == ComponentTypeKind::Variant);
  return typeDef_->schema().as<ComponentVariantCaseVector>();
}

ComponentType ComponentType::asList() const {
  MOZ_RELEASE_ASSERT(kind() == ComponentTypeKind::List);
  return typeDef_->schema().as<ComponentType>();
}

const ComponentTypeVector& ComponentType::asTuple() const {
  MOZ_RELEASE_ASSERT(kind() == ComponentTypeKind::Tuple);
  return typeDef_->schema().as<ComponentTypeVector>();
}

const CacheableNameVector& ComponentType::asFlags() const {
  MOZ_RELEASE_ASSERT(kind() == ComponentTypeKind::Flags);
  return typeDef_->schema().as<CacheableNameVector>();
}

const CacheableNameVector& ComponentType::asEnum() const {
  MOZ_RELEASE_ASSERT(kind() == ComponentTypeKind::Enum);
  return typeDef_->schema().as<CacheableNameVector>();
}

ComponentType ComponentType::asOption() const {
  MOZ_RELEASE_ASSERT(kind() == ComponentTypeKind::Option);
  return typeDef_->schema().as<ComponentType>();
}

ComponentResultType ComponentType::asResult() const {
  MOZ_RELEASE_ASSERT(kind() == ComponentTypeKind::Result);
  return typeDef_->schema().as<ComponentResultType>();
}

ComponentType ComponentType::asOwn() const {
  MOZ_RELEASE_ASSERT(kind() == ComponentTypeKind::Own);
  return typeDef_->schema().as<ComponentType>();
}

ComponentType ComponentType::asBorrow() const {
  MOZ_RELEASE_ASSERT(kind() == ComponentTypeKind::Borrow);
  return typeDef_->schema().as<ComponentType>();
}

const ComponentFuncType& ComponentType::asFunc() const {
  MOZ_RELEASE_ASSERT(kind() == ComponentTypeKind::Func);
  return typeDef_->schema().as<ComponentFuncType>();
}

const ComponentResourceType& ComponentType::asResource() const {
  MOZ_RELEASE_ASSERT(kind() == ComponentTypeKind::Resource ||
                     kind() == ComponentTypeKind::SubResource);
  return typeDef_->schema().as<ComponentResourceType>();
}

void ComponentType::setSubResourceInterface(JS::WebIDLInterface* iface) const {
  MOZ_RELEASE_ASSERT(kind() == ComponentTypeKind::SubResource);
  typeDef_->mutableSchema().as<ComponentResourceType>().setInterface(iface);
}

bool ComponentType::structurallyEqual(const ComponentType& a,
                                      const ComponentType& b) {
  return a.kind() == b.kind() &&
         ComponentTypeDef::structurallyEqual(*a.typeDef_, *b.typeDef_);
}

bool ComponentTypeDef::structurallyEqual(const ComponentTypeDef& a,
                                         const ComponentTypeDef& b) {
  return a.schema().match(
      [&](const mozilla::Nothing&) {
        return b.schema().is<mozilla::Nothing>();
      },
      [&](const ComponentType& aType) {
        if (!b.schema().is<ComponentType>()) {
          return false;
        }
        const ComponentType& bType = b.schema().as<ComponentType>();
        return aType == bType;
      },
      [&](const ComponentRecordFieldVector& aFields) {
        if (!b.schema().is<ComponentRecordFieldVector>()) {
          return false;
        }
        const ComponentRecordFieldVector& bFields =
            b.schema().as<ComponentRecordFieldVector>();

        if (aFields.length() != bFields.length()) {
          return false;
        }
        for (size_t i = 0; i < aFields.length(); i++) {
          if (aFields[i] != bFields[i]) {
            return false;
          }
        }
        return true;
      },
      [&](const ComponentVariantCaseVector& aCases) {
        if (!b.schema().is<ComponentVariantCaseVector>()) {
          return false;
        }
        const ComponentVariantCaseVector& bCases =
            b.schema().as<ComponentVariantCaseVector>();

        if (aCases.length() != bCases.length()) {
          return false;
        }
        for (size_t i = 0; i < aCases.length(); i++) {
          if (aCases[i] != bCases[i]) {
            return false;
          }
        }
        return true;
      },
      [&](const ComponentTypeVector& aTypes) {
        if (!b.schema().is<ComponentTypeVector>()) {
          return false;
        }
        const ComponentTypeVector& bTypes =
            b.schema().as<ComponentTypeVector>();

        if (aTypes.length() != bTypes.length()) {
          return false;
        }
        for (size_t i = 0; i < aTypes.length(); i++) {
          if (aTypes[i] != bTypes[i]) {
            return false;
          }
        }
        return true;
      },
      [&](const CacheableNameVector& aLabels) {
        if (!b.schema().is<CacheableNameVector>()) {
          return false;
        }
        const CacheableNameVector& bLabels =
            b.schema().as<CacheableNameVector>();

        if (aLabels.length() != bLabels.length()) {
          return false;
        }
        for (size_t i = 0; i < aLabels.length(); i++) {
          if (aLabels[i] != bLabels[i]) {
            return false;
          }
        }
        return true;
      },
      [&](const ComponentResultType& aResult) {
        if (!b.schema().is<ComponentResultType>()) {
          return false;
        }
        const ComponentResultType& bResult =
            b.schema().as<ComponentResultType>();
        return ComponentResultType::equals(aResult, bResult);
      },
      [&](const ComponentFuncType& aFunc) {
        if (!b.schema().is<ComponentFuncType>()) {
          return false;
        }
        const ComponentFuncType& bFunc = b.schema().as<ComponentFuncType>();
        return aFunc == bFunc;
      },
      [&](const ComponentResourceType& a) {
        // This method never considers resource types to be equal because this
        // is the wrong place to check for that kind of equality. Two
        // canonicalized ComponentTypes for resource types may be equal, because
        // they point at the same ComponentTypeDef (by pointer equality), but
        // this method has no concept of that.
        return false;
      });
}

[[nodiscard]] static HashNumber AddComponentTypeToHash(HashNumber hash,
                                                       ComponentType type) {
  hash = mozilla::AddToHash(hash, type.kind());
  hash = mozilla::AddToHash(hash, type.typeDef().get());
  return hash;
}

[[nodiscard]] static HashNumber AddMaybeComponentTypeToHash(
    HashNumber hash, mozilla::Maybe<ComponentType> type) {
  hash = mozilla::AddToHash(hash, type.isSome());
  if (type.isSome()) {
    hash = AddComponentTypeToHash(hash, *type);
  }
  return hash;
}

static HashNumber HashName(const CacheableName& name) {
  return mozilla::HashString(name.utf8Bytes().data(),
                             name.utf8Bytes().Length());
}

HashNumber ComponentTypeHasher::hash(const ComponentType& t) {
  HashNumber hash = 0;
  hash = mozilla::AddToHash(hash, t.kind());

  // Primitives and resource types should not appear here; this is caught by the
  // default case.
  switch (t.kind()) {
    case ComponentTypeKind::Record: {
      const ComponentRecordFieldVector& fields = t.asRecord();
      for (const ComponentRecordField& f : fields) {
        hash = mozilla::AddToHash(hash, HashName(f.name));
        hash = AddComponentTypeToHash(hash, f.type);
      }
    } break;
    case ComponentTypeKind::Variant: {
      const ComponentVariantCaseVector& cases = t.asVariant();
      for (const ComponentVariantCase& c : cases) {
        hash = mozilla::AddToHash(hash, HashName(c.name));
        hash = AddMaybeComponentTypeToHash(hash, c.type);
      }
    } break;
    case ComponentTypeKind::List: {
      hash = AddComponentTypeToHash(hash, t.asList());
    } break;
    case ComponentTypeKind::Tuple: {
      const ComponentTypeVector& types = t.asTuple();
      for (const ComponentType& t : types) {
        hash = AddComponentTypeToHash(hash, t);
      }
    } break;
    case ComponentTypeKind::Flags: {
      const CacheableNameVector& labels = t.asFlags();
      for (const CacheableName& label : labels) {
        hash = mozilla::AddToHash(hash, HashName(label));
      }
    } break;
    case ComponentTypeKind::Enum: {
      const CacheableNameVector& cases = t.asEnum();
      for (const CacheableName& c : cases) {
        hash = mozilla::AddToHash(hash, HashName(c));
      }
    } break;
    case ComponentTypeKind::Option: {
      hash = AddComponentTypeToHash(hash, t.asOption());
    } break;
    case ComponentTypeKind::Result: {
      const ComponentResultType& rt = t.asResult();
      hash = AddMaybeComponentTypeToHash(hash, rt.type);
      hash = AddMaybeComponentTypeToHash(hash, rt.errorType);
    } break;
    case ComponentTypeKind::Own: {
      hash = AddComponentTypeToHash(hash, t.asOwn());
    } break;
    case ComponentTypeKind::Borrow: {
      hash = AddComponentTypeToHash(hash, t.asBorrow());
    } break;
    case ComponentTypeKind::Func: {
      const ComponentFuncType& ft = t.asFunc();
      MOZ_ASSERT(ft.paramTypes.length() == ft.paramNames.length());
      for (size_t i = 0; i < ft.paramTypes.length(); i++) {
        hash = mozilla::AddToHash(hash, HashName(ft.paramNames[i]));
        hash = AddComponentTypeToHash(hash, ft.paramTypes[i]);
      }
      hash = AddMaybeComponentTypeToHash(hash, ft.resultType);
    } break;
    case ComponentTypeKind::Component:
    case ComponentTypeKind::Instance:
      // TODO(wasm-cm): Component and instance types not yet implemented
      MOZ_CRASH();
    default:
      MOZ_CRASH("should have been excluded from hashing");
  }

  return hash;
}
bool ComponentTypeHasher::match(const ComponentType& a,
                                const ComponentType& b) {
  // (eq i) bounds should be resolved to a unique type on type construction.
  MOZ_ASSERT(a.kind() != ComponentTypeKind::Eq);
  MOZ_ASSERT(b.kind() != ComponentTypeKind::Eq);

  // Primitives and resource types should be special-cased during
  // canonicalization and should therefore never end up here.
  MOZ_ASSERT(!ComponentTypeKindIsPrimitive(a.kind()) &&
             a.kind() != ComponentTypeKind::Resource &&
             a.kind() != ComponentTypeKind::SubResource);
  MOZ_ASSERT(!ComponentTypeKindIsPrimitive(b.kind()) &&
             b.kind() != ComponentTypeKind::Resource &&
             b.kind() != ComponentTypeKind::SubResource);

  return ComponentType::structurallyEqual(a, b);
}

bool ComponentCanonicalTypeSet::canonicalize(const ComponentType& type,
                                             ComponentType* canonicalized) {
  MOZ_RELEASE_ASSERT(type.isValid());

  // Primitives compare trivially and require no additional storage, therefore
  // they do not need to be explicitly stored.
  if (ComponentTypeKindIsPrimitive(type.kind())) {
    MOZ_RELEASE_ASSERT(!type.typeDef());
    *canonicalized = type;
    return true;
  }
  MOZ_RELEASE_ASSERT(type.typeDef());

  // Resource types retain their uniqueness by skipping canonicalization.
  if (type.kind() == ComponentTypeKind::Resource ||
      type.kind() == ComponentTypeKind::SubResource) {
    *canonicalized = type;
    return true;
  }

  // All other types are hashed and deduplicated structurally. As long as all
  // types are canonicalized as they are parsed, this means that pointer
  // equality of a type's ComponentTypeDef is equivalent to structural equality.
  auto addPtr = canonicalTypes_.lookupForAdd(type);
  if (addPtr) {
    *canonicalized = *addPtr;
    return true;
  }
  if (!canonicalTypes_.add(addPtr, type)) {
    return false;
  }
  *canonicalized = type;
  return true;
}

MOZ_RUNINIT static ExclusiveData<ComponentCanonicalTypeSet>
    sComponentCanonicalTypeSet(mutexid::WasmComponentCanonicalTypeSet);

bool wasm::CanonicalizeComponentType(const ComponentType& type,
                                     ComponentType* canonicalized) {
  ExclusiveData<ComponentCanonicalTypeSet>::Guard locked =
      sComponentCanonicalTypeSet.lock();
  return locked->canonicalize(type, canonicalized);
}

void wasm::PurgeComponentCanonicalTypes() {
  ExclusiveData<ComponentCanonicalTypeSet>::Guard locked =
      sComponentCanonicalTypeSet.lock();
  locked->canonicalTypes_.clearAndCompact();
}

mozilla::Maybe<FuncType> wasm::FlattenFuncType(
    const ComponentFuncType& funcType, CanonMode mode, bool* memoryRequired,
    bool* reallocRequired, bool* tooDeep) {
  ValTypeVector params;
  ValTypeVector results;

  bool paramsHaveStringsOrLists = false;
  bool resultsHaveStringsOrLists = false;
  if (!FlattenTypes(funcType.paramTypes, &params, &paramsHaveStringsOrLists,
                    tooDeep, /*depth=*/0)) {
    return mozilla::Nothing();
  }
  if (funcType.resultType.isSome()) {
    if (!FlattenType(funcType.resultType.ref(), &results,
                     &resultsHaveStringsOrLists, tooDeep, /*depth=*/0)) {
      return mozilla::Nothing();
    }
  }

  // String or list params require realloc on lift and memory on lower.
  // String or list results require memory on lift and realloc on lower.
  if ((mode == CanonMode::Lift && resultsHaveStringsOrLists) ||
      (mode == CanonMode::Lower && paramsHaveStringsOrLists)) {
    *memoryRequired = true;
  }
  if ((mode == CanonMode::Lift && paramsHaveStringsOrLists) ||
      (mode == CanonMode::Lower && resultsHaveStringsOrLists)) {
    *reallocRequired = true;
  }

  // Regardless of lifting or lowering, if there are too many flattened params,
  // values must be passed in memory. Replace the params with a single pointer
  // to memory.
  if (params.length() > MaxFlatParams) {
    params.clear();
    if (!params.append(ValType::i32())) {
      return mozilla::Nothing();
    }

    // Spilled params must be alloced by the host on lifted functions, whereas
    // on lowered functions they are allocated by the module and read by the
    // host.
    if (mode == CanonMode::Lift) {
      *reallocRequired = true;
    } else {
      *memoryRequired = true;
    }
  }

  // If there are too many flattened results (e.g. returning a record), the
  // results must also be passed in memory. When lifting a core func, the core
  // module does the obvious thing and allocates space for the results,
  // returning a pointer. When lowering a component func, however, returning a
  // pointer would imply that the component callee has to call `realloc` to get
  // such a pointer, which is dumb when the core caller could just allocate
  // stack space for it, so instead the canonical ABI chooses to make the
  // results an out param.
  if (results.length() > MaxFlatResults) {
    if (mode == CanonMode::Lift) {
      results.clear();
      if (!results.append(ValType::i32())) {
        return mozilla::Nothing();
      }
    } else {
      if (!params.append(ValType::i32())) {
        return mozilla::Nothing();
      }
      results.clear();
    }

    *memoryRequired = true;
  }

  // If realloc is required, memory is always also required.
  if (*reallocRequired) {
    *memoryRequired = true;
  }

  return mozilla::Some(FuncType(std::move(params), std::move(results)));
}

bool wasm::FlattenTypes(const ComponentTypeVector& types, ValTypeVector* result,
                        bool* hasStringsOrLists, bool* tooDeep,
                        uint32_t depth) {
  // Pre-reserve at least enough space for a bunch of primitives. We still may
  // exceed the capacity reserved here but at least we can avoid a little bit of
  // allocation. (Appends after this point are not to be considered infallible.)
  if (!result->reserve(types.length())) {
    return false;
  }

  for (const ComponentType& t : types) {
    if (!FlattenType(t, result, hasStringsOrLists, tooDeep, depth)) {
      return false;
    }
  }

  return true;
}

static ValType JoinVariantValType(ValType a, ValType b) {
  MOZ_ASSERT(a.isNumber() && b.isNumber());
  if (a == b) {
    return a;
  } else if ((a == ValType::i32() && b == ValType::f32()) ||
             (a == ValType::f32() && b == ValType::i32())) {
    return ValType::i32();
  } else {
    return ValType::i64();
  }
}

bool wasm::FlattenType(const ComponentType& type, ValTypeVector* result,
                       bool* hasStringsOrLists, bool* tooDeep, uint32_t depth) {
  if (depth > MaxComponentFlatteningDepth) {
    *tooDeep = true;
    return false;
  }
  depth += 1;

  switch (type.kind()) {
    // Simple primitives
    case ComponentTypeKind::Bool:
    case ComponentTypeKind::U8:
    case ComponentTypeKind::U16:
    case ComponentTypeKind::U32:
    case ComponentTypeKind::S8:
    case ComponentTypeKind::S16:
    case ComponentTypeKind::S32:
    case ComponentTypeKind::Char:
    case ComponentTypeKind::Flags:
    case ComponentTypeKind::Enum:
    case ComponentTypeKind::Own:
    case ComponentTypeKind::Borrow: {
      if (!result->append(ValType::i32())) {
        return false;
      }
    } break;
    case ComponentTypeKind::U64:
    case ComponentTypeKind::S64: {
      if (!result->append(ValType::i64())) {
        return false;
      }
    } break;
    case ComponentTypeKind::F32: {
      if (!result->append(ValType::f32())) {
        return false;
      }
    } break;
    case ComponentTypeKind::F64: {
      if (!result->append(ValType::f64())) {
        return false;
      }
    } break;

    // Strings are always two i32's
    case ComponentTypeKind::String: {
      *hasStringsOrLists = true;
      if (!result->append(ValType::i32())) {
        return false;
      }
      if (!result->append(ValType::i32())) {
        return false;
      }
    } break;

    // Compound types have dedicated logic. Note that our data storage for some
    // types disagrees with the categories in the canonical ABI explainer, e.g.
    // we represent tuples as a vector of value types, not a record.
    case ComponentTypeKind::List: {
      *hasStringsOrLists = true;
      // This will have to change when support is added for fixed-length lists.
      if (!result->append(ValType::i32())) {
        return false;
      }
      if (!result->append(ValType::i32())) {
        return false;
      }
    } break;
    case ComponentTypeKind::Record: {
      if (!FlattenRecord(type.asRecord(), result, hasStringsOrLists, tooDeep,
                         depth)) {
        return false;
      }
    } break;
    case ComponentTypeKind::Tuple: {
      if (!FlattenTypes(type.asTuple(), result, hasStringsOrLists, tooDeep,
                        depth)) {
        return false;
      }
    } break;
    case ComponentTypeKind::Variant: {
      // Flatten the discriminant
      if (!result->append(ValType::i32())) {
        return false;
      }

      // Flatten all the cases (overlapped, with joins)
      const ComponentVariantCaseVector& cases = type.asVariant();
      size_t startIndex = result->length();
      for (const ComponentVariantCase& case_ : cases) {
        if (!case_.type) {
          continue;
        }

        ValTypeVector caseFlattened;
        if (!FlattenType(*case_.type, &caseFlattened, hasStringsOrLists,
                         tooDeep, depth)) {
          return false;
        }
        for (size_t i = 0; i < caseFlattened.length(); i++) {
          size_t existingIndex = startIndex + i;
          if (existingIndex < result->length()) {
            // Join the new type with the existing one.
            (*result)[existingIndex] =
                JoinVariantValType((*result)[existingIndex], caseFlattened[i]);
          } else {
            // Append the new type to the overall list.
            if (!result->append(caseFlattened[i])) {
              return false;
            }
          }
        }
      }
    } break;
    case ComponentTypeKind::Option: {
      ComponentType inner = type.asOption();
      if (!result->append(ValType::i32())) {
        return false;
      }
      if (!FlattenType(inner, result, hasStringsOrLists, tooDeep, depth)) {
        return false;
      }
    } break;
    case ComponentTypeKind::Result: {
      ComponentResultType inner = type.asResult();
      // Result types are encoded just like a variant with two cases, but each
      // case may or may not have a type.

      // Discriminant
      if (!result->append(ValType::i32())) {
        return false;
      }

      // Payload(s)
      size_t startIndex = result->length();
      if (inner.type.isSome()) {
        if (!FlattenType(*inner.type, result, hasStringsOrLists, tooDeep,
                         depth)) {
          return false;
        }
      }
      if (inner.errorType.isSome()) {
        ValTypeVector errorFlattened;
        if (!FlattenType(*inner.errorType, &errorFlattened, hasStringsOrLists,
                         tooDeep, depth)) {
          return false;
        }
        for (size_t i = 0; i < errorFlattened.length(); i++) {
          size_t existingIndex = startIndex + i;
          if (existingIndex < result->length()) {
            (*result)[existingIndex] =
                JoinVariantValType((*result)[existingIndex], errorFlattened[i]);
          } else {
            if (!result->append(errorFlattened[i])) {
              return false;
            }
          }
        }
      }
    } break;

    default:
      MOZ_CRASH("should have been rejected when the func type was validated");
  }

  return true;
}

bool wasm::FlattenRecord(const ComponentRecordFieldVector& fields,
                         ValTypeVector* result, bool* hasStringsOrLists,
                         bool* tooDeep, uint32_t depth) {
  for (const ComponentRecordField& field : fields) {
    if (!FlattenType(field.type, result, hasStringsOrLists, tooDeep, depth)) {
      return false;
    }
  }

  return true;
}

bool wasm::FlattenTypes(const ComponentTypeVector& types,
                        ValTypeVector* result) {
  bool hasStringsOrLists = false;
  bool tooDeep = false;
  return FlattenTypes(types, result, &hasStringsOrLists, &tooDeep,
                      /*depth=*/0);
}

bool wasm::FlattenType(const ComponentType& type, ValTypeVector* result) {
  bool hasStringsOrLists = false;
  bool tooDeep = false;
  return FlattenType(type, result, &hasStringsOrLists, &tooDeep, /*depth=*/0);
}

JSObject* ComponentResourceHandle::reflectToJS(JSContext* cx) const {
  if (isNative) {
    return nativeIface->reflect(cx, nativePtr);
  }
  JS::Rooted<JSObject*> obj(cx, wrappedObj);
  if (!JS_WrapObject(cx, &obj)) {
    return nullptr;
  }
  return obj;
}

bool JS::WebIDLContext::memoryBytes(uint8_t** base, size_t* len) const {
  *base = memoryBase_;
  // TODO: only works with unshared memory
  const WasmArrayRawBuffer* rawBuf =
      WasmArrayRawBuffer::fromDataPtr(memoryBase_);
  *len = rawBuf->byteLength();
  return true;
}

void JS::WebIDLContext::resolveRealloc(JS::Handle<JSObject*> realloc) {
  reallocInstance_ = nullptr;
  reallocFuncIndex_ = 0;
  if (!realloc) {
    return;
  }
  JSObject* target = js::UncheckedUnwrap(realloc);
  if (!target->is<JSFunction>()) {
    return;
  }
  JSFunction* fun = &target->as<JSFunction>();
  if (!fun->isWasm()) {
    return;
  }
  reallocInstance_ = &fun->wasmInstance();
  reallocFuncIndex_ = fun->wasmFuncIndex();
}

bool JS::WebIDLContext::callRealloc(uint32_t origPtr, uint32_t origSize,
                                    uint32_t align, uint32_t newSize,
                                    uint32_t* outPtr) const {
  if (!realloc_) {
    JS_ReportErrorASCII(cx_, "canonical ABI operation requires realloc");
    return false;
  }

  // `realloc_` is a (possibly cross-compartment-wrapped) wasm export: call it
  // directly via Instance::callExport, skipping the generic JS::Call ->
  // WasmCall dispatch and its argument/result boxing.
  if (reallocInstance_) {
    AutoRealmUnchecked ar(cx_, reallocInstance_->realm());
    FixedInvokeArgs<4> args(cx_);
    args[0].setNumber(origPtr);
    args[1].setNumber(origSize);
    args[2].setNumber(align);
    args[3].setNumber(newSize);
    if (!reallocInstance_->callExport(cx_, reallocFuncIndex_, args)) {
      return false;
    }
    return JS::ToUint32(cx_, args.rval(), outPtr);
  }

  JS::RootedValueArray<4> argv(cx_);
  argv[0].setNumber(origPtr);
  argv[1].setNumber(origSize);
  argv[2].setNumber(align);
  argv[3].setNumber(newSize);

  JS::RootedValue thisv(cx_, JS::UndefinedValue());
  JS::RootedValue reallocVal(cx_, JS::ObjectValue(*realloc_));
  JS::RootedValue rval(cx_);
  if (!JS::Call(cx_, thisv, reallocVal, argv, &rval)) {
    return false;
  }

  return JS::ToUint32(cx_, rval, outPtr);
}

void JS::WebIDLContext::trace(JSTracer* trc) {
  JS::TraceEdge(trc, &memory_, "WebIDLContext memory");
  JS::TraceEdge(trc, &realloc_, "WebIDLContext realloc");
}

static bool ReportCanonicalABIUnsupported(JSContext* cx, const char* what) {
  JS_ReportErrorASCII(cx, "canonical ABI: %s not yet supported", what);
  return false;
}

static uint32_t AlignTo(uint32_t ptr, uint32_t align) {
  return (ptr + align - 1) & ~(align - 1);
}

static uint32_t DiscriminantSize(uint32_t numCases) {
  MOZ_ASSERT(numCases > 0);
  if (numCases <= 256) {
    return 1;
  }
  if (numCases <= 65536) {
    return 2;
  }
  return 4;
}

static uint32_t FlagsSize(uint32_t numLabels) {
  MOZ_ASSERT(numLabels > 0 && numLabels <= 32);
  if (numLabels <= 8) {
    return 1;
  }
  if (numLabels <= 16) {
    return 2;
  }
  return 4;
}

static uint32_t Alignment(const ComponentType& type);
static uint32_t ElemSize(const ComponentType& type);

// Defined near the other canon lower internals below; forward-declared here
// since real instantiation's lower trampoline needs it well before that.
static bool DefaultWebIDLOperationType(JSContext* cx, bool hasResult,
                                       JS::WebIDLOperationType* out);

static uint32_t RecordAlignment(const ComponentTypeVector& items) {
  uint32_t a = 1;
  for (const ComponentType& t : items) {
    a = std::max(a, Alignment(t));
  }
  return a;
}

static uint32_t RecordAlignment(const ComponentRecordFieldVector& fields) {
  uint32_t a = 1;
  for (const ComponentRecordField& f : fields) {
    a = std::max(a, Alignment(f.type));
  }
  return a;
}

static uint32_t RecordElemSize(const ComponentTypeVector& items) {
  uint32_t s = 0;
  for (const ComponentType& t : items) {
    s = AlignTo(s, Alignment(t));
    s += ElemSize(t);
  }
  return AlignTo(s, RecordAlignment(items));
}

static uint32_t RecordElemSize(const ComponentRecordFieldVector& fields) {
  uint32_t s = 0;
  for (const ComponentRecordField& f : fields) {
    s = AlignTo(s, Alignment(f.type));
    s += ElemSize(f.type);
  }
  return AlignTo(s, RecordAlignment(fields));
}

static uint32_t NumVariantLikeCases(const ComponentType& type) {
  if (type.kind() == ComponentTypeKind::Result) {
    return 2;
  }
  MOZ_ASSERT(type.kind() == ComponentTypeKind::Variant);
  return uint32_t(type.asVariant().length());
}

static mozilla::Maybe<ComponentType> VariantLikeCaseType(
    const ComponentType& type, uint32_t caseIndex) {
  if (type.kind() == ComponentTypeKind::Result) {
    ComponentResultType result = type.asResult();
    return caseIndex == 0 ? result.type : result.errorType;
  }
  MOZ_ASSERT(type.kind() == ComponentTypeKind::Variant);
  return type.asVariant()[caseIndex].type;
}

static uint32_t VariantLikeMaxPayloadAlign(const ComponentType& type) {
  uint32_t numCases = NumVariantLikeCases(type);
  uint32_t maxAlign = 1;
  for (uint32_t i = 0; i < numCases; i++) {
    if (mozilla::Maybe<ComponentType> caseType = VariantLikeCaseType(type, i)) {
      maxAlign = std::max(maxAlign, Alignment(*caseType));
    }
  }
  return maxAlign;
}

// Case count/type/label for a variant-like build-time source: Variant,
// Result, or a nested Option (which has no ComponentValueData::VariantData
// of its own).
static uint32_t NumBuildVariantCases(const ComponentType& type) {
  if (type.kind() == ComponentTypeKind::Option) {
    return 2;
  }
  return NumVariantLikeCases(type);
}

static mozilla::Maybe<ComponentType> BuildVariantCaseType(
    const ComponentType& type, uint32_t caseIndex) {
  if (type.kind() == ComponentTypeKind::Option) {
    return caseIndex == 0 ? mozilla::Nothing() : mozilla::Some(type.asOption());
  }
  return VariantLikeCaseType(type, caseIndex);
}

static mozilla::Span<const char> BuildVariantCaseLabel(
    const ComponentType& type, uint32_t caseIndex) {
  if (type.kind() == ComponentTypeKind::Variant) {
    return type.asVariant()[caseIndex].name.utf8Bytes();
  }
  if (type.kind() == ComponentTypeKind::Result) {
    static const char kOk[] = "ok";
    static const char kError[] = "error";
    return caseIndex == 0 ? mozilla::Span<const char>(kOk, 2)
                          : mozilla::Span<const char>(kError, 5);
  }
  MOZ_ASSERT(type.kind() == ComponentTypeKind::Option);
  static const char kNone[] = "none";
  static const char kSome[] = "some";
  return caseIndex == 0 ? mozilla::Span<const char>(kNone, 4)
                        : mozilla::Span<const char>(kSome, 4);
}

static uint32_t BuildVariantMaxPayloadAlign(const ComponentType& type) {
  if (type.kind() == ComponentTypeKind::Option) {
    return Alignment(type.asOption());
  }
  return VariantLikeMaxPayloadAlign(type);
}

static uint32_t Alignment(const ComponentType& type) {
  switch (type.kind()) {
    case ComponentTypeKind::Bool:
    case ComponentTypeKind::S8:
    case ComponentTypeKind::U8:
      return 1;
    case ComponentTypeKind::S16:
    case ComponentTypeKind::U16:
      return 2;
    case ComponentTypeKind::S32:
    case ComponentTypeKind::U32:
    case ComponentTypeKind::Char:
    case ComponentTypeKind::F32:
    // Handles are opaque u32 table indices.
    case ComponentTypeKind::Own:
    case ComponentTypeKind::Borrow:
      return 4;
    case ComponentTypeKind::S64:
    case ComponentTypeKind::U64:
    case ComponentTypeKind::F64:
      return 8;
    // Pointer-sized (ptr,len) pairs. Fixed-length lists are not supported.
    case ComponentTypeKind::String:
    case ComponentTypeKind::List:
      return 4;
    case ComponentTypeKind::Record:
      return RecordAlignment(type.asRecord());
    case ComponentTypeKind::Tuple:
      return RecordAlignment(type.asTuple());
    case ComponentTypeKind::Flags:
      return FlagsSize(type.asFlags().length());
    // Despecializes to a no-payload variant; alignment is just the
    // discriminant's.
    case ComponentTypeKind::Enum:
      return DiscriminantSize(type.asEnum().length());
    // Despecializes to a 2-case variant {none, some(inner)}.
    case ComponentTypeKind::Option:
      return std::max(DiscriminantSize(2), Alignment(type.asOption()));
    case ComponentTypeKind::Variant:
    case ComponentTypeKind::Result:
      return std::max(DiscriminantSize(NumVariantLikeCases(type)),
                      VariantLikeMaxPayloadAlign(type));
    default:
      MOZ_CRASH("unsupported component type for canonical ABI layout");
  }
}

static uint32_t ElemSize(const ComponentType& type) {
  switch (type.kind()) {
    case ComponentTypeKind::Bool:
    case ComponentTypeKind::S8:
    case ComponentTypeKind::U8:
      return 1;
    case ComponentTypeKind::S16:
    case ComponentTypeKind::U16:
      return 2;
    case ComponentTypeKind::S32:
    case ComponentTypeKind::U32:
    case ComponentTypeKind::Char:
    case ComponentTypeKind::F32:
    // Handles are opaque u32 table indices.
    case ComponentTypeKind::Own:
    case ComponentTypeKind::Borrow:
      return 4;
    case ComponentTypeKind::S64:
    case ComponentTypeKind::U64:
    case ComponentTypeKind::F64:
      return 8;
    case ComponentTypeKind::String:
    case ComponentTypeKind::List:
      return 8;  // two i32/ptr-sized fields: (ptr, len)
    case ComponentTypeKind::Record:
      return RecordElemSize(type.asRecord());
    case ComponentTypeKind::Tuple:
      return RecordElemSize(type.asTuple());
    case ComponentTypeKind::Flags:
      return FlagsSize(type.asFlags().length());
    case ComponentTypeKind::Enum:
      return DiscriminantSize(type.asEnum().length());
    case ComponentTypeKind::Option: {
      ComponentType inner = type.asOption();
      uint32_t discSize = DiscriminantSize(2);
      uint32_t s = AlignTo(discSize, Alignment(inner));
      s += ElemSize(inner);
      return AlignTo(s, Alignment(type));
    }
    case ComponentTypeKind::Variant:
    case ComponentTypeKind::Result: {
      uint32_t numCases = NumVariantLikeCases(type);
      uint32_t discSize = DiscriminantSize(numCases);
      uint32_t maxAlign = VariantLikeMaxPayloadAlign(type);
      uint32_t maxSize = 0;
      for (uint32_t i = 0; i < numCases; i++) {
        if (mozilla::Maybe<ComponentType> caseType =
                VariantLikeCaseType(type, i)) {
          maxSize = std::max(maxSize, ElemSize(*caseType));
        }
      }
      uint32_t s = AlignTo(discSize, maxAlign) + maxSize;
      return AlignTo(s, Alignment(type));
    }
    default:
      MOZ_CRASH("unsupported component type for canonical ABI layout");
  }
}

// Values are always little-endian, per the canonical ABI. NaN payloads are
// not canonicalized or scrambled (a deliberate simplification for this
// prototype).

static bool Store(WebIDLContext& ctx, const ComponentValue& value,
                  const ComponentType& type, uint32_t ptr);
static bool Load(WebIDLContext& ctx, uint32_t ptr, const ComponentType& type,
                 ComponentValue* out);

static bool WriteBytes(WebIDLContext& ctx, uint32_t ptr, const void* data,
                       uint32_t nbytes) {
  uint8_t* base;
  size_t len;
  if (!ctx.memoryBytes(&base, &len)) {
    return false;
  }
  if (uint64_t(ptr) + nbytes > len) {
    JS_ReportErrorASCII(ctx.cx(), "canonical ABI: store out of bounds");
    return false;
  }
  memcpy(base + ptr, data, nbytes);
  return true;
}

static bool ReadBytes(WebIDLContext& ctx, uint32_t ptr, void* data,
                      uint32_t nbytes) {
  uint8_t* base;
  size_t len;
  if (!ctx.memoryBytes(&base, &len)) {
    return false;
  }
  if (uint64_t(ptr) + nbytes > len) {
    JS_ReportErrorASCII(ctx.cx(), "canonical ABI: load out of bounds");
    return false;
  }
  memcpy(data, base + ptr, nbytes);
  return true;
}

// Fixed-size variants of WriteBytes/ReadBytes, for callers that know nbytes
// at compile time - lets the compiler emit a single load/store instead of a
// memcpy call.
template <uint32_t NBytes>
static bool WriteBytes(WebIDLContext& ctx, uint32_t ptr, const void* data) {
  uint8_t* base;
  size_t len;
  if (!ctx.memoryBytes(&base, &len)) {
    return false;
  }
  if (uint64_t(ptr) + NBytes > len) {
    JS_ReportErrorASCII(ctx.cx(), "canonical ABI: store out of bounds");
    return false;
  }
  memcpy(base + ptr, data, NBytes);
  return true;
}

template <uint32_t NBytes>
static bool ReadBytes(WebIDLContext& ctx, uint32_t ptr, void* data) {
  uint8_t* base;
  size_t len;
  if (!ctx.memoryBytes(&base, &len)) {
    return false;
  }
  if (uint64_t(ptr) + NBytes > len) {
    JS_ReportErrorASCII(ctx.cx(), "canonical ABI: load out of bounds");
    return false;
  }
  memcpy(data, base + ptr, NBytes);
  return true;
}

static bool WriteUint(WebIDLContext& ctx, uint32_t ptr, uint64_t value,
                      uint32_t nbytes) {
  uint8_t buf[8];
  for (uint32_t i = 0; i < nbytes; i++) {
    buf[i] = uint8_t(value >> (8 * i));
  }
  return WriteBytes(ctx, ptr, buf, nbytes);
}

static bool ReadUint(WebIDLContext& ctx, uint32_t ptr, uint32_t nbytes,
                     uint64_t* out) {
  uint8_t buf[8];
  if (!ReadBytes(ctx, ptr, buf, nbytes)) {
    return false;
  }
  uint64_t v = 0;
  for (uint32_t i = 0; i < nbytes; i++) {
    v |= uint64_t(buf[i]) << (8 * i);
  }
  *out = v;
  return true;
}

// Fixed-size variants of WriteUint/ReadUint, dispatched on the integer type
// T rather than a runtime byte count - for callers where the value's width
// is known at compile time.
template <typename T>
static bool WriteUint(WebIDLContext& ctx, uint32_t ptr, T value) {
  static_assert(std::is_unsigned_v<T>, "WriteUint<T> requires an unsigned T");
  if constexpr (sizeof(T) == 1) {
    return WriteBytes<1>(ctx, ptr, &value);
  } else {
    T le = mozilla::NativeEndian::swapToLittleEndian(value);
    return WriteBytes<sizeof(T)>(ctx, ptr, &le);
  }
}

template <typename T>
static bool ReadUint(WebIDLContext& ctx, uint32_t ptr, T* out) {
  static_assert(std::is_unsigned_v<T>, "ReadUint<T> requires an unsigned T");
  if constexpr (sizeof(T) == 1) {
    return ReadBytes<1>(ctx, ptr, out);
  } else {
    T le;
    if (!ReadBytes<sizeof(T)>(ctx, ptr, &le)) {
      return false;
    }
    *out = mozilla::NativeEndian::swapFromLittleEndian(le);
    return true;
  }
}

static bool AllocateAndStoreStringBytes(WebIDLContext& ctx,
                                        const ComponentValue& value,
                                        uint32_t* outPtr, uint32_t* outLen) {
  const auto& utf8 = value.data()->asString().utf8;
  uint32_t len = uint32_t(utf8.length());
  uint32_t strPtr;
  if (!ctx.callRealloc(0, 0, 1, len, &strPtr)) {
    return false;
  }
  if (len > 0 && !WriteBytes(ctx, strPtr, utf8.begin(), len)) {
    return false;
  }
  *outPtr = strPtr;
  *outLen = len;
  return true;
}

static bool StoreString(WebIDLContext& ctx, const ComponentValue& value,
                        uint32_t ptr) {
  uint32_t strPtr, len;
  if (!AllocateAndStoreStringBytes(ctx, value, &strPtr, &len)) {
    return false;
  }
  return WriteUint<uint32_t>(ctx, ptr, strPtr) &&
         WriteUint<uint32_t>(ctx, ptr + 4, len);
}

static bool LoadStringFromRange(WebIDLContext& ctx, uint32_t strPtr,
                                uint32_t len, const ComponentType& type,
                                ComponentValue* out) {
  uint8_t* base;
  size_t memLen;
  if (!ctx.memoryBytes(&base, &memLen)) {
    return false;
  }
  if (uint64_t(strPtr) + len > memLen) {
    JS_ReportErrorASCII(ctx.cx(), "canonical ABI: string out of bounds");
    return false;
  }

  ComponentValueData::StringData sd;
  if (!sd.utf8.append(reinterpret_cast<const char*>(base + strPtr), len)) {
    ReportOutOfMemory(ctx.cx());
    return false;
  }
  RefPtr<ComponentValueData> data = js_new<ComponentValueData>(std::move(sd));
  if (!data) {
    ReportOutOfMemory(ctx.cx());
    return false;
  }
  *out = ComponentValue::makeComposite(type, std::move(data));
  return true;
}

static bool LoadString(WebIDLContext& ctx, uint32_t ptr,
                       const ComponentType& type, ComponentValue* out) {
  uint32_t strPtr, len;
  if (!ReadUint(ctx, ptr, &strPtr) || !ReadUint(ctx, ptr + 4, &len)) {
    return false;
  }
  return LoadStringFromRange(ctx, strPtr, len, type, out);
}

static bool AllocateAndStoreListElements(WebIDLContext& ctx,
                                         const ComponentValue& value,
                                         const ComponentType& elemType,
                                         uint32_t* outPtr, uint32_t* outN) {
  const auto& elements = value.data()->asElements().elements;
  uint32_t n = uint32_t(elements.length());
  uint32_t elemSize = ElemSize(elemType);
  uint32_t elemAlign = Alignment(elemType);

  // TODO(wasm-cm): Trap instead of overflowing if n * elemSize exceeds
  // REALLOC_I32_MAX, per the canonical ABI.
  uint32_t listPtr;
  if (!ctx.callRealloc(0, 0, elemAlign, n * elemSize, &listPtr)) {
    return false;
  }
  for (uint32_t i = 0; i < n; i++) {
    if (!Store(ctx, elements[i], elemType, listPtr + i * elemSize)) {
      return false;
    }
  }
  *outPtr = listPtr;
  *outN = n;
  return true;
}

static bool StoreList(WebIDLContext& ctx, const ComponentValue& value,
                      const ComponentType& elemType, uint32_t ptr) {
  uint32_t listPtr, n;
  if (!AllocateAndStoreListElements(ctx, value, elemType, &listPtr, &n)) {
    return false;
  }
  return WriteUint<uint32_t>(ctx, ptr, listPtr) &&
         WriteUint<uint32_t>(ctx, ptr + 4, n);
}

static bool LoadListFromRange(WebIDLContext& ctx, uint32_t listPtr, uint32_t n,
                              const ComponentType& elemType,
                              const ComponentType& type, ComponentValue* out) {
  uint32_t elemSize = ElemSize(elemType);

  ComponentValueData::ElementsData ed;
  if (!ed.elements.reserve(n)) {
    ReportOutOfMemory(ctx.cx());
    return false;
  }
  for (uint32_t i = 0; i < n; i++) {
    ComponentValue elem;
    if (!Load(ctx, listPtr + i * elemSize, elemType, &elem)) {
      return false;
    }
    if (!ed.elements.append(elem)) {
      ReportOutOfMemory(ctx.cx());
      return false;
    }
  }
  RefPtr<ComponentValueData> data = js_new<ComponentValueData>(std::move(ed));
  if (!data) {
    ReportOutOfMemory(ctx.cx());
    return false;
  }
  *out = ComponentValue::makeComposite(type, std::move(data));
  return true;
}

static bool LoadList(WebIDLContext& ctx, uint32_t ptr,
                     const ComponentType& elemType, const ComponentType& type,
                     ComponentValue* out) {
  uint32_t listPtr, n;
  if (!ReadUint(ctx, ptr, &listPtr) || !ReadUint(ctx, ptr + 4, &n)) {
    return false;
  }
  return LoadListFromRange(ctx, listPtr, n, elemType, type, out);
}

static bool StoreElements(WebIDLContext& ctx,
                          const ComponentValueVector& values,
                          const ComponentTypeVector& types, uint32_t ptr) {
  MOZ_ASSERT(values.length() == types.length());
  uint32_t p = ptr;
  for (size_t i = 0; i < types.length(); i++) {
    p = AlignTo(p, Alignment(types[i]));
    if (!Store(ctx, values[i], types[i], p)) {
      return false;
    }
    p += ElemSize(types[i]);
  }
  return true;
}

static bool LoadElements(WebIDLContext& ctx, uint32_t ptr,
                         const ComponentTypeVector& types,
                         ComponentValueVector* out) {
  uint32_t p = ptr;
  for (size_t i = 0; i < types.length(); i++) {
    p = AlignTo(p, Alignment(types[i]));
    ComponentValue v;
    if (!Load(ctx, p, types[i], &v)) {
      return false;
    }
    if (!out->append(v)) {
      ReportOutOfMemory(ctx.cx());
      return false;
    }
    p += ElemSize(types[i]);
  }
  return true;
}

static bool StoreRecord(WebIDLContext& ctx, const ComponentValueVector& values,
                        const ComponentRecordFieldVector& fields,
                        uint32_t ptr) {
  MOZ_ASSERT(values.length() == fields.length());
  uint32_t p = ptr;
  for (size_t i = 0; i < fields.length(); i++) {
    p = AlignTo(p, Alignment(fields[i].type));
    if (!Store(ctx, values[i], fields[i].type, p)) {
      return false;
    }
    p += ElemSize(fields[i].type);
  }
  return true;
}

static bool LoadRecord(WebIDLContext& ctx, uint32_t ptr,
                       const ComponentRecordFieldVector& fields,
                       ComponentValueVector* out) {
  uint32_t p = ptr;
  for (size_t i = 0; i < fields.length(); i++) {
    p = AlignTo(p, Alignment(fields[i].type));
    ComponentValue v;
    if (!Load(ctx, p, fields[i].type, &v)) {
      return false;
    }
    if (!out->append(v)) {
      ReportOutOfMemory(ctx.cx());
      return false;
    }
    p += ElemSize(fields[i].type);
  }
  return true;
}

static uint64_t PackFlags(
    const mozilla::Vector<bool, 0, SystemAllocPolicy>& bits) {
  uint64_t packed = 0;
  for (size_t i = 0; i < bits.length(); i++) {
    if (bits[i]) {
      packed |= uint64_t(1) << i;
    }
  }
  return packed;
}

static bool UnpackFlags(JSContext* cx, uint64_t packed,
                        const CacheableNameVector& labels,
                        const ComponentType& type, ComponentValue* out) {
  ComponentValueData::FlagsData fd;
  if (!fd.bits.reserve(labels.length())) {
    ReportOutOfMemory(cx);
    return false;
  }
  for (size_t i = 0; i < labels.length(); i++) {
    if (!fd.bits.append((packed & (uint64_t(1) << i)) != 0)) {
      ReportOutOfMemory(cx);
      return false;
    }
  }
  RefPtr<ComponentValueData> data = js_new<ComponentValueData>(std::move(fd));
  if (!data) {
    ReportOutOfMemory(cx);
    return false;
  }
  *out = ComponentValue::makeComposite(type, std::move(data));
  return true;
}

static bool StoreFlags(WebIDLContext& ctx, const ComponentValue& value,
                       const CacheableNameVector& labels, uint32_t ptr) {
  const auto& bits = value.data()->asFlags().bits;
  MOZ_ASSERT(bits.length() == labels.length());
  return WriteUint(ctx, ptr, PackFlags(bits),
                   FlagsSize(uint32_t(labels.length())));
}

static bool LoadFlags(WebIDLContext& ctx, uint32_t ptr,
                      const CacheableNameVector& labels,
                      const ComponentType& type, ComponentValue* out) {
  uint64_t packed;
  if (!ReadUint(ctx, ptr, FlagsSize(uint32_t(labels.length())), &packed)) {
    return false;
  }
  return UnpackFlags(ctx.cx(), packed, labels, type, out);
}

static uint32_t VariantLikeCaseIndex(const ComponentValue& value) {
  if (value.type().kind() == ComponentTypeKind::Result) {
    return value.data()->asResult().isOk ? 0 : 1;
  }
  MOZ_ASSERT(value.type().kind() == ComponentTypeKind::Variant);
  return value.data()->asVariant().caseIndex;
}

static const mozilla::Maybe<ComponentValue>& VariantLikePayload(
    const ComponentValue& value) {
  if (value.type().kind() == ComponentTypeKind::Result) {
    return value.data()->asResult().payload;
  }
  MOZ_ASSERT(value.type().kind() == ComponentTypeKind::Variant);
  return value.data()->asVariant().payload;
}

static bool MakeVariantLikeValue(JSContext* cx, const ComponentType& type,
                                 uint32_t caseIndex,
                                 mozilla::Maybe<ComponentValue>&& payload,
                                 ComponentValue* out) {
  RefPtr<ComponentValueData> data;
  if (type.kind() == ComponentTypeKind::Result) {
    data = js_new<ComponentValueData>(
        ComponentValueData::ResultData{caseIndex == 0, std::move(payload)});
  } else {
    MOZ_ASSERT(type.kind() == ComponentTypeKind::Variant);
    data = js_new<ComponentValueData>(
        ComponentValueData::VariantData{caseIndex, std::move(payload)});
  }
  if (!data) {
    ReportOutOfMemory(cx);
    return false;
  }
  *out = ComponentValue::makeComposite(type, std::move(data));
  return true;
}

static bool Store(WebIDLContext& ctx, const ComponentValue& value,
                  const ComponentType& type, uint32_t ptr) {
  switch (type.kind()) {
    case ComponentTypeKind::Bool:
      return WriteUint<uint8_t>(ctx, ptr, value.asBool() ? 1 : 0);
    case ComponentTypeKind::S8:
      return WriteUint<uint8_t>(ctx, ptr, uint8_t(value.asSigned()));
    case ComponentTypeKind::U8:
      return WriteUint<uint8_t>(ctx, ptr, uint8_t(value.asUnsigned()));
    case ComponentTypeKind::S16:
      return WriteUint<uint16_t>(ctx, ptr, uint16_t(value.asSigned()));
    case ComponentTypeKind::U16:
      return WriteUint<uint16_t>(ctx, ptr, uint16_t(value.asUnsigned()));
    case ComponentTypeKind::S32:
      return WriteUint<uint32_t>(ctx, ptr, uint32_t(value.asSigned()));
    case ComponentTypeKind::U32:
    case ComponentTypeKind::Char:
    case ComponentTypeKind::Own:
    case ComponentTypeKind::Borrow:
      return WriteUint<uint32_t>(ctx, ptr, uint32_t(value.asUnsigned()));
    case ComponentTypeKind::S64:
      return WriteUint<uint64_t>(ctx, ptr, uint64_t(value.asSigned()));
    case ComponentTypeKind::U64:
      return WriteUint<uint64_t>(ctx, ptr, value.asUnsigned());
    case ComponentTypeKind::F32:
      return WriteUint<uint32_t>(ctx, ptr,
                                 mozilla::BitwiseCast<uint32_t>(value.asF32()));
    case ComponentTypeKind::F64:
      return WriteUint<uint64_t>(ctx, ptr,
                                 mozilla::BitwiseCast<uint64_t>(value.asF64()));
    case ComponentTypeKind::String:
      return StoreString(ctx, value, ptr);
    case ComponentTypeKind::List:
      return StoreList(ctx, value, type.asList(), ptr);
    case ComponentTypeKind::Record:
      return StoreRecord(ctx, value.data()->asElements().elements,
                         type.asRecord(), ptr);
    case ComponentTypeKind::Tuple:
      return StoreElements(ctx, value.data()->asElements().elements,
                           type.asTuple(), ptr);
    case ComponentTypeKind::Flags:
      return StoreFlags(ctx, value, type.asFlags(), ptr);
    case ComponentTypeKind::Enum:
      return WriteUint(ctx, ptr, value.asUnsigned(),
                       DiscriminantSize(uint32_t(type.asEnum().length())));
    case ComponentTypeKind::Option: {
      ComponentType inner = type.asOption();
      const auto& opt = value.data()->asOption();
      uint32_t discSize = DiscriminantSize(2);
      if (opt.payload.isNothing()) {
        return WriteUint(ctx, ptr, 0, discSize);
      }
      uint32_t payloadPtr = AlignTo(ptr + discSize, Alignment(inner));
      return WriteUint(ctx, ptr, 1, discSize) &&
             Store(ctx, *opt.payload, inner, payloadPtr);
    }
    case ComponentTypeKind::Variant:
    case ComponentTypeKind::Result: {
      uint32_t discSize = DiscriminantSize(NumVariantLikeCases(type));
      uint32_t caseIndex = VariantLikeCaseIndex(value);
      if (!WriteUint(ctx, ptr, caseIndex, discSize)) {
        return false;
      }
      mozilla::Maybe<ComponentType> caseType =
          VariantLikeCaseType(type, caseIndex);
      if (!caseType) {
        return true;
      }
      uint32_t payloadPtr =
          AlignTo(ptr + discSize, VariantLikeMaxPayloadAlign(type));
      return Store(ctx, *VariantLikePayload(value), *caseType, payloadPtr);
    }
    default:
      return ReportCanonicalABIUnsupported(ctx.cx(), "this component type");
  }
}

// Assumes all bits above nbytes*8 are already zero, as returned by ReadUint.
static int64_t SignExtend(uint64_t v, uint32_t nbytes) {
  uint32_t bits = nbytes * 8;
  uint64_t signBit = uint64_t(1) << (bits - 1);
  return int64_t((v ^ signBit) - signBit);
}

static bool Load(WebIDLContext& ctx, uint32_t ptr, const ComponentType& type,
                 ComponentValue* out) {
  switch (type.kind()) {
    case ComponentTypeKind::Bool: {
      uint8_t v;
      if (!ReadUint(ctx, ptr, &v)) {
        return false;
      }
      *out = ComponentValue::makeBool(type, v != 0);
      return true;
    }
    case ComponentTypeKind::S8: {
      uint8_t v;
      if (!ReadUint(ctx, ptr, &v)) {
        return false;
      }
      *out = ComponentValue::makeSigned(type, SignExtend(v, 1));
      return true;
    }
    case ComponentTypeKind::S16: {
      uint16_t v;
      if (!ReadUint(ctx, ptr, &v)) {
        return false;
      }
      *out = ComponentValue::makeSigned(type, SignExtend(v, 2));
      return true;
    }
    case ComponentTypeKind::S32: {
      uint32_t v;
      if (!ReadUint(ctx, ptr, &v)) {
        return false;
      }
      *out = ComponentValue::makeSigned(type, SignExtend(v, 4));
      return true;
    }
    case ComponentTypeKind::S64: {
      uint64_t v;
      if (!ReadUint(ctx, ptr, &v)) {
        return false;
      }
      *out = ComponentValue::makeSigned(type, SignExtend(v, 8));
      return true;
    }
    case ComponentTypeKind::U8: {
      uint8_t v;
      if (!ReadUint(ctx, ptr, &v)) {
        return false;
      }
      *out = ComponentValue::makeUnsigned(type, v);
      return true;
    }
    case ComponentTypeKind::U16: {
      uint16_t v;
      if (!ReadUint(ctx, ptr, &v)) {
        return false;
      }
      *out = ComponentValue::makeUnsigned(type, v);
      return true;
    }
    case ComponentTypeKind::U32:
    case ComponentTypeKind::Char:
    case ComponentTypeKind::Own:
    case ComponentTypeKind::Borrow: {
      uint32_t v;
      if (!ReadUint(ctx, ptr, &v)) {
        return false;
      }
      *out = ComponentValue::makeUnsigned(type, v);
      return true;
    }
    case ComponentTypeKind::U64: {
      uint64_t v;
      if (!ReadUint(ctx, ptr, &v)) {
        return false;
      }
      *out = ComponentValue::makeUnsigned(type, v);
      return true;
    }
    case ComponentTypeKind::F32: {
      uint32_t bits;
      if (!ReadUint(ctx, ptr, &bits)) {
        return false;
      }
      *out = ComponentValue::makeF32(type, mozilla::BitwiseCast<float>(bits));
      return true;
    }
    case ComponentTypeKind::F64: {
      uint64_t bits;
      if (!ReadUint(ctx, ptr, &bits)) {
        return false;
      }
      *out = ComponentValue::makeF64(type, mozilla::BitwiseCast<double>(bits));
      return true;
    }
    case ComponentTypeKind::String:
      return LoadString(ctx, ptr, type, out);
    case ComponentTypeKind::List:
      return LoadList(ctx, ptr, type.asList(), type, out);
    case ComponentTypeKind::Record: {
      ComponentValueData::ElementsData ed;
      if (!LoadRecord(ctx, ptr, type.asRecord(), &ed.elements)) {
        return false;
      }
      RefPtr<ComponentValueData> data =
          js_new<ComponentValueData>(std::move(ed));
      if (!data) {
        ReportOutOfMemory(ctx.cx());
        return false;
      }
      *out = ComponentValue::makeComposite(type, std::move(data));
      return true;
    }
    case ComponentTypeKind::Tuple: {
      ComponentValueData::ElementsData ed;
      if (!LoadElements(ctx, ptr, type.asTuple(), &ed.elements)) {
        return false;
      }
      RefPtr<ComponentValueData> data =
          js_new<ComponentValueData>(std::move(ed));
      if (!data) {
        ReportOutOfMemory(ctx.cx());
        return false;
      }
      *out = ComponentValue::makeComposite(type, std::move(data));
      return true;
    }
    case ComponentTypeKind::Flags:
      return LoadFlags(ctx, ptr, type.asFlags(), type, out);
    case ComponentTypeKind::Enum: {
      uint32_t numLabels = uint32_t(type.asEnum().length());
      uint64_t idx;
      if (!ReadUint(ctx, ptr, DiscriminantSize(numLabels), &idx)) {
        return false;
      }
      if (idx >= numLabels) {
        JS_ReportErrorASCII(ctx.cx(),
                            "canonical ABI: invalid enum discriminant");
        return false;
      }
      *out = ComponentValue::makeUnsigned(type, idx);
      return true;
    }
    case ComponentTypeKind::Option: {
      ComponentType inner = type.asOption();
      uint32_t discSize = DiscriminantSize(2);
      uint64_t disc;
      if (!ReadUint(ctx, ptr, discSize, &disc)) {
        return false;
      }
      if (disc > 1) {
        JS_ReportErrorASCII(ctx.cx(),
                            "canonical ABI: invalid option discriminant");
        return false;
      }

      ComponentValueData::OptionData od;
      if (disc != 0) {
        uint32_t payloadPtr = AlignTo(ptr + discSize, Alignment(inner));
        ComponentValue innerVal;
        if (!Load(ctx, payloadPtr, inner, &innerVal)) {
          return false;
        }
        od.payload = mozilla::Some(innerVal);
      }
      RefPtr<ComponentValueData> data =
          js_new<ComponentValueData>(std::move(od));
      if (!data) {
        ReportOutOfMemory(ctx.cx());
        return false;
      }
      *out = ComponentValue::makeComposite(type, std::move(data));
      return true;
    }
    case ComponentTypeKind::Variant:
    case ComponentTypeKind::Result: {
      uint32_t numCases = NumVariantLikeCases(type);
      uint32_t discSize = DiscriminantSize(numCases);
      uint64_t discRaw;
      if (!ReadUint(ctx, ptr, discSize, &discRaw)) {
        return false;
      }
      if (discRaw >= numCases) {
        JS_ReportErrorASCII(ctx.cx(),
                            "canonical ABI: invalid variant discriminant");
        return false;
      }
      uint32_t caseIndex = uint32_t(discRaw);
      mozilla::Maybe<ComponentType> caseType =
          VariantLikeCaseType(type, caseIndex);
      mozilla::Maybe<ComponentValue> payload;
      if (caseType) {
        uint32_t payloadPtr =
            AlignTo(ptr + discSize, VariantLikeMaxPayloadAlign(type));
        ComponentValue inner;
        if (!Load(ctx, payloadPtr, *caseType, &inner)) {
          return false;
        }
        payload = mozilla::Some(inner);
      }
      return MakeVariantLikeValue(ctx.cx(), type, caseIndex, std::move(payload),
                                  out);
    }
    default:
      return ReportCanonicalABIUnsupported(ctx.cx(), "this component type");
  }
}

// Every supported type flattens to i32/i64/f32/f64 (no GC refs). variant/
// result need to join overlapping case slots; see CoerceFlatValUp/Down
// below.

static bool LowerFlat(WebIDLContext& ctx, const ComponentValue& value,
                      const ComponentType& type, RootedValVector& out);
static bool LiftFlat(WebIDLContext& ctx, HandleValVector flat, size_t* index,
                     const ComponentType& type, ComponentValue* out);

static Val ZeroVal(ValType type) {
  switch (type.kind()) {
    case ValType::Kind::I32:
      return Val(uint32_t(0));
    case ValType::Kind::I64:
      return Val(uint64_t(0));
    case ValType::Kind::F32:
      return Val(float(0));
    case ValType::Kind::F64:
      return Val(double(0));
    default:
      MOZ_CRASH("unexpected core value type in canonical ABI");
  }
}

static int64_t LiftFlatSigned(uint64_t raw, uint32_t nbytes) {
  uint32_t bits = nbytes * 8;
  uint64_t masked = bits >= 64 ? raw : raw & ((uint64_t(1) << bits) - 1);
  return SignExtend(masked, nbytes);
}
static uint64_t LiftFlatUnsigned(uint64_t raw, uint32_t nbytes) {
  uint32_t bits = nbytes * 8;
  return bits >= 64 ? raw : raw & ((uint64_t(1) << bits) - 1);
}

static const Val& NextFlat(HandleValVector flat, size_t* index) {
  return flat[(*index)++].get();
}

// A case's flat value may need widening to fit the slot the joined signature
// declared for it (the canonical ABI's i32/f32->i32 and other->i64 join
// rules).
static Val CoerceFlatValUp(const Val& have, ValType want) {
  ValType haveType = have.type();
  if (haveType == want) {
    return have;
  }
  if (haveType == ValType::f32() && want == ValType::i32()) {
    return Val(mozilla::BitwiseCast<uint32_t>(have.f32()));
  }
  if (haveType == ValType::i32() && want == ValType::i64()) {
    return Val(uint64_t(have.i32()));
  }
  if (haveType == ValType::f32() && want == ValType::i64()) {
    return Val(uint64_t(mozilla::BitwiseCast<uint32_t>(have.f32())));
  }
  if (haveType == ValType::f64() && want == ValType::i64()) {
    return Val(mozilla::BitwiseCast<uint64_t>(have.f64()));
  }
  MOZ_CRASH("unexpected variant flat value coercion");
}

static Val CoerceFlatValDown(const Val& have, ValType want) {
  ValType haveType = have.type();
  if (haveType == want) {
    return have;
  }
  if (haveType == ValType::i32() && want == ValType::f32()) {
    return Val(mozilla::BitwiseCast<float>(have.i32()));
  }
  if (haveType == ValType::i64() && want == ValType::i32()) {
    return Val(uint32_t(have.i64()));
  }
  if (haveType == ValType::i64() && want == ValType::f32()) {
    return Val(mozilla::BitwiseCast<float>(uint32_t(have.i64())));
  }
  if (haveType == ValType::i64() && want == ValType::f64()) {
    return Val(mozilla::BitwiseCast<double>(have.i64()));
  }
  MOZ_CRASH("unexpected variant flat value coercion");
}

static bool LowerFlatElements(WebIDLContext& ctx,
                              const ComponentValueVector& values,
                              const ComponentTypeVector& types,
                              RootedValVector& out) {
  for (size_t i = 0; i < types.length(); i++) {
    if (!LowerFlat(ctx, values[i], types[i], out)) {
      return false;
    }
  }
  return true;
}

static bool LowerFlatRecord(WebIDLContext& ctx,
                            const ComponentValueVector& values,
                            const ComponentRecordFieldVector& fields,
                            RootedValVector& out) {
  for (size_t i = 0; i < fields.length(); i++) {
    if (!LowerFlat(ctx, values[i], fields[i].type, out)) {
      return false;
    }
  }
  return true;
}

static bool LiftFlatElements(WebIDLContext& ctx, HandleValVector flat,
                             size_t* index, const ComponentTypeVector& types,
                             ComponentValueVector* out) {
  for (size_t i = 0; i < types.length(); i++) {
    ComponentValue v;
    if (!LiftFlat(ctx, flat, index, types[i], &v)) {
      return false;
    }
    if (!out->append(v)) {
      ReportOutOfMemory(ctx.cx());
      return false;
    }
  }
  return true;
}

static bool LiftFlatRecord(WebIDLContext& ctx, HandleValVector flat,
                           size_t* index,
                           const ComponentRecordFieldVector& fields,
                           ComponentValueVector* out) {
  for (size_t i = 0; i < fields.length(); i++) {
    ComponentValue v;
    if (!LiftFlat(ctx, flat, index, fields[i].type, &v)) {
      return false;
    }
    if (!out->append(v)) {
      ReportOutOfMemory(ctx.cx());
      return false;
    }
  }
  return true;
}

static bool LowerFlat(WebIDLContext& ctx, const ComponentValue& value,
                      const ComponentType& type, RootedValVector& out) {
  switch (type.kind()) {
    case ComponentTypeKind::Bool:
      return out.append(Val(uint32_t(value.asBool() ? 1 : 0)));
    case ComponentTypeKind::S8:
    case ComponentTypeKind::S16:
    case ComponentTypeKind::S32:
      return out.append(Val(uint32_t(uint64_t(value.asSigned()))));
    case ComponentTypeKind::U8:
    case ComponentTypeKind::U16:
    case ComponentTypeKind::U32:
    case ComponentTypeKind::Char:
    case ComponentTypeKind::Own:
    case ComponentTypeKind::Borrow:
      return out.append(Val(uint32_t(value.asUnsigned())));
    case ComponentTypeKind::S64:
      return out.append(Val(uint64_t(value.asSigned())));
    case ComponentTypeKind::U64:
      return out.append(Val(value.asUnsigned()));
    case ComponentTypeKind::F32:
      return out.append(Val(value.asF32()));
    case ComponentTypeKind::F64:
      return out.append(Val(value.asF64()));
    case ComponentTypeKind::String: {
      uint32_t strPtr, len;
      if (!AllocateAndStoreStringBytes(ctx, value, &strPtr, &len)) {
        return false;
      }
      return out.append(Val(strPtr)) && out.append(Val(len));
    }
    case ComponentTypeKind::List: {
      uint32_t listPtr, n;
      if (!AllocateAndStoreListElements(ctx, value, type.asList(), &listPtr,
                                        &n)) {
        return false;
      }
      return out.append(Val(listPtr)) && out.append(Val(n));
    }
    case ComponentTypeKind::Record:
      return LowerFlatRecord(ctx, value.data()->asElements().elements,
                             type.asRecord(), out);
    case ComponentTypeKind::Tuple:
      return LowerFlatElements(ctx, value.data()->asElements().elements,
                               type.asTuple(), out);
    case ComponentTypeKind::Flags:
      return out.append(Val(uint32_t(PackFlags(value.data()->asFlags().bits))));
    case ComponentTypeKind::Enum:
      return out.append(Val(uint32_t(value.asUnsigned())));
    case ComponentTypeKind::Option: {
      ComponentType inner = type.asOption();
      const auto& opt = value.data()->asOption();
      if (opt.payload.isSome()) {
        return out.append(Val(uint32_t(1))) &&
               LowerFlat(ctx, *opt.payload, inner, out);
      }
      if (!out.append(Val(uint32_t(0)))) {
        return false;
      }
      // The core func signature is value-independent, so a `none` still
      // occupies exactly as many flat slots as `inner` would.
      ValTypeVector innerFlat;
      if (!FlattenType(inner, &innerFlat)) {
        return false;
      }
      for (const ValType& vt : innerFlat) {
        if (!out.append(ZeroVal(vt))) {
          return false;
        }
      }
      return true;
    }
    case ComponentTypeKind::Variant:
    case ComponentTypeKind::Result: {
      if (!out.append(Val(VariantLikeCaseIndex(value)))) {
        return false;
      }

      ValTypeVector joinedFlat;
      if (!FlattenType(type, &joinedFlat)) {
        return false;
      }

      uint32_t caseIndex = VariantLikeCaseIndex(value);
      mozilla::Maybe<ComponentType> caseType =
          VariantLikeCaseType(type, caseIndex);
      size_t payloadCount = 0;
      if (caseType) {
        RootedValVector caseFlat(ctx.cx());
        if (!LowerFlat(ctx, *VariantLikePayload(value), *caseType, caseFlat)) {
          return false;
        }
        for (size_t i = 0; i < caseFlat.length(); i++) {
          if (!out.append(CoerceFlatValUp(caseFlat[i], joinedFlat[1 + i]))) {
            return false;
          }
        }
        payloadCount = caseFlat.length();
      }
      // The core func signature is value-independent, so every case still
      // occupies exactly as many flat slots as the widest one.
      for (size_t i = payloadCount + 1; i < joinedFlat.length(); i++) {
        if (!out.append(ZeroVal(joinedFlat[i]))) {
          return false;
        }
      }
      return true;
    }
    default:
      return ReportCanonicalABIUnsupported(ctx.cx(), "this component type");
  }
}

static bool LiftFlat(WebIDLContext& ctx, HandleValVector flat, size_t* index,
                     const ComponentType& type, ComponentValue* out) {
  switch (type.kind()) {
    case ComponentTypeKind::Bool: {
      uint32_t v = NextFlat(flat, index).i32();
      *out = ComponentValue::makeBool(type, v != 0);
      return true;
    }
    case ComponentTypeKind::S8:
    case ComponentTypeKind::S16:
    case ComponentTypeKind::S32: {
      uint32_t raw = NextFlat(flat, index).i32();
      *out =
          ComponentValue::makeSigned(type, LiftFlatSigned(raw, ElemSize(type)));
      return true;
    }
    case ComponentTypeKind::S64: {
      uint64_t raw = NextFlat(flat, index).i64();
      *out = ComponentValue::makeSigned(type, LiftFlatSigned(raw, 8));
      return true;
    }
    case ComponentTypeKind::U8:
    case ComponentTypeKind::U16:
    case ComponentTypeKind::U32:
    case ComponentTypeKind::Char:
    case ComponentTypeKind::Own:
    case ComponentTypeKind::Borrow: {
      uint32_t raw = NextFlat(flat, index).i32();
      *out = ComponentValue::makeUnsigned(
          type, LiftFlatUnsigned(raw, ElemSize(type)));
      return true;
    }
    case ComponentTypeKind::U64: {
      uint64_t raw = NextFlat(flat, index).i64();
      *out = ComponentValue::makeUnsigned(type, raw);
      return true;
    }
    case ComponentTypeKind::F32:
      *out = ComponentValue::makeF32(type, NextFlat(flat, index).f32());
      return true;
    case ComponentTypeKind::F64:
      *out = ComponentValue::makeF64(type, NextFlat(flat, index).f64());
      return true;
    case ComponentTypeKind::String: {
      uint32_t strPtr = NextFlat(flat, index).i32();
      uint32_t len = NextFlat(flat, index).i32();
      return LoadStringFromRange(ctx, strPtr, len, type, out);
    }
    case ComponentTypeKind::List: {
      uint32_t listPtr = NextFlat(flat, index).i32();
      uint32_t n = NextFlat(flat, index).i32();
      return LoadListFromRange(ctx, listPtr, n, type.asList(), type, out);
    }
    case ComponentTypeKind::Record: {
      ComponentValueData::ElementsData ed;
      if (!LiftFlatRecord(ctx, flat, index, type.asRecord(), &ed.elements)) {
        return false;
      }
      RefPtr<ComponentValueData> data =
          js_new<ComponentValueData>(std::move(ed));
      if (!data) {
        ReportOutOfMemory(ctx.cx());
        return false;
      }
      *out = ComponentValue::makeComposite(type, std::move(data));
      return true;
    }
    case ComponentTypeKind::Tuple: {
      ComponentValueData::ElementsData ed;
      if (!LiftFlatElements(ctx, flat, index, type.asTuple(), &ed.elements)) {
        return false;
      }
      RefPtr<ComponentValueData> data =
          js_new<ComponentValueData>(std::move(ed));
      if (!data) {
        ReportOutOfMemory(ctx.cx());
        return false;
      }
      *out = ComponentValue::makeComposite(type, std::move(data));
      return true;
    }
    case ComponentTypeKind::Flags: {
      uint32_t packed = NextFlat(flat, index).i32();
      return UnpackFlags(ctx.cx(), packed, type.asFlags(), type, out);
    }
    case ComponentTypeKind::Enum: {
      uint32_t numLabels = uint32_t(type.asEnum().length());
      uint32_t idx = NextFlat(flat, index).i32();
      if (idx >= numLabels) {
        JS_ReportErrorASCII(ctx.cx(),
                            "canonical ABI: invalid enum discriminant");
        return false;
      }
      *out = ComponentValue::makeUnsigned(type, idx);
      return true;
    }
    case ComponentTypeKind::Option: {
      ComponentType inner = type.asOption();
      uint32_t disc = NextFlat(flat, index).i32();
      if (disc > 1) {
        JS_ReportErrorASCII(ctx.cx(),
                            "canonical ABI: invalid option discriminant");
        return false;
      }
      ComponentValueData::OptionData od;
      if (disc != 0) {
        ComponentValue innerVal;
        if (!LiftFlat(ctx, flat, index, inner, &innerVal)) {
          return false;
        }
        od.payload = mozilla::Some(innerVal);
      } else {
        ValTypeVector innerFlat;
        if (!FlattenType(inner, &innerFlat)) {
          return false;
        }
        *index += innerFlat.length();
      }
      RefPtr<ComponentValueData> data =
          js_new<ComponentValueData>(std::move(od));
      if (!data) {
        ReportOutOfMemory(ctx.cx());
        return false;
      }
      *out = ComponentValue::makeComposite(type, std::move(data));
      return true;
    }
    case ComponentTypeKind::Variant:
    case ComponentTypeKind::Result: {
      ValTypeVector joinedFlat;
      if (!FlattenType(type, &joinedFlat)) {
        return false;
      }
      size_t payloadSlots = joinedFlat.length() - 1;

      uint32_t numCases = NumVariantLikeCases(type);
      uint32_t caseIndex = NextFlat(flat, index).i32();
      if (caseIndex >= numCases) {
        JS_ReportErrorASCII(ctx.cx(),
                            "canonical ABI: invalid variant discriminant");
        return false;
      }

      mozilla::Maybe<ComponentType> caseType =
          VariantLikeCaseType(type, caseIndex);
      mozilla::Maybe<ComponentValue> payload;
      if (caseType) {
        ValTypeVector caseFlatTypes;
        if (!FlattenType(*caseType, &caseFlatTypes)) {
          return false;
        }
        RootedValVector coerced(ctx.cx());
        if (!coerced.reserve(caseFlatTypes.length())) {
          ReportOutOfMemory(ctx.cx());
          return false;
        }
        for (size_t i = 0; i < caseFlatTypes.length(); i++) {
          coerced.infallibleAppend(
              CoerceFlatValDown(NextFlat(flat, index), caseFlatTypes[i]));
        }
        size_t innerIndex = 0;
        ComponentValue innerVal;
        if (!LiftFlat(ctx, coerced, &innerIndex, *caseType, &innerVal)) {
          return false;
        }
        payload = mozilla::Some(innerVal);
        *index += payloadSlots - caseFlatTypes.length();
      } else {
        *index += payloadSlots;
      }

      return MakeVariantLikeValue(ctx.cx(), type, caseIndex, std::move(payload),
                                  out);
    }
    default:
      return ReportCanonicalABIUnsupported(ctx.cx(), "this component type");
  }
}

static bool LowerFlatValues(
    WebIDLContext& ctx, const ComponentTypeVector& types,
    const ComponentValueVector& values, uint32_t maxFlat,
    RootedValVector& coreOut,
    mozilla::Maybe<uint32_t> callerProvidedPtr = mozilla::Nothing()) {
  MOZ_ASSERT(types.length() == values.length());
  coreOut.clear();

  ValTypeVector flatTypes;
  if (!FlattenTypes(types, &flatTypes)) {
    return false;
  }

  if (flatTypes.length() > maxFlat) {
    uint32_t ptr;
    if (callerProvidedPtr) {
      ptr = *callerProvidedPtr;
    } else {
      uint32_t align = RecordAlignment(types);
      uint32_t size = RecordElemSize(types);
      if (!ctx.callRealloc(0, 0, align, size, &ptr)) {
        return false;
      }
    }
    if (!StoreElements(ctx, values, types, ptr)) {
      return false;
    }
    // A caller-provided pointer is an out-param the caller already knows
    // about (see FlattenFuncType's Lower comment); the flat value(s) it would
    // otherwise carry are empty in that case, not the pointer itself.
    return callerProvidedPtr ? true : coreOut.append(Val(ptr));
  }

  return LowerFlatElements(ctx, values, types, coreOut);
}

static bool LiftFlatValues(WebIDLContext& ctx, const ComponentTypeVector& types,
                           HandleValVector coreVals, uint32_t maxFlat,
                           ComponentValueVector* valuesOut) {
  ValTypeVector flatTypes;
  if (!FlattenTypes(types, &flatTypes)) {
    return false;
  }

  if (flatTypes.length() > maxFlat) {
    MOZ_RELEASE_ASSERT(coreVals.length() == 1);
    uint32_t ptr = coreVals[0].get().i32();
    return LoadElements(ctx, ptr, types, valuesOut);
  }

  size_t index = 0;
  for (const ComponentType& t : types) {
    ComponentValue v;
    if (!LiftFlat(ctx, coreVals, &index, t, &v)) {
      return false;
    }
    if (!valuesOut->append(v)) {
      ReportOutOfMemory(ctx.cx());
      return false;
    }
  }
  MOZ_ASSERT(index == coreVals.length());
  return true;
}

static bool PrepareCanonABIParams(WebIDLContext& ctx,
                                  const ComponentFuncType& funcType,
                                  const ComponentValueVector& params,
                                  RootedValVector& coreParamsOut) {
  return LowerFlatValues(ctx, funcType.paramTypes, params, MaxFlatParams,
                         coreParamsOut);
}

static bool ReceiveCanonABIResults(WebIDLContext& ctx,
                                   const ComponentFuncType& funcType,
                                   HandleValVector coreResults,
                                   ComponentValueVector* resultsOut) {
  ComponentTypeVector resultTypes;
  if (funcType.resultType.isSome() &&
      !resultTypes.append(funcType.resultType.ref())) {
    ReportOutOfMemory(ctx.cx());
    return false;
  }
  return LiftFlatValues(ctx, resultTypes, coreResults, MaxFlatResults,
                        resultsOut);
}

static bool PrepareCanonABIResults(
    WebIDLContext& ctx, const ComponentFuncType& funcType,
    const ComponentValueVector& results, RootedValVector& coreResultsOut,
    mozilla::Maybe<uint32_t> callerProvidedPtr = mozilla::Nothing()) {
  ComponentTypeVector resultTypes;
  if (funcType.resultType.isSome() &&
      !resultTypes.append(funcType.resultType.ref())) {
    ReportOutOfMemory(ctx.cx());
    return false;
  }
  return LowerFlatValues(ctx, resultTypes, results, MaxFlatResults,
                         coreResultsOut, callerProvidedPtr);
}

/* virtual */
JSObject* Component::createObject(JSContext* cx) const {
  if (!GlobalObject::ensureConstructor(cx, cx->global(), JSProto_WebAssembly)) {
    return nullptr;
  }

  JS::RootedVector<JSString*> parameterStrings(cx);
  JS::RootedVector<Value> parameterArgs(cx);
  bool canCompileStrings = false;
  if (!cx->isRuntimeCodeGenEnabled(JS::RuntimeCode::WASM, nullptr,
                                   JS::CompilationType::Undefined,
                                   parameterStrings, nullptr, parameterArgs,
                                   NullHandleValue, &canCompileStrings)) {
    return nullptr;
  }
  if (!canCompileStrings) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_CSP_BLOCKED_WASM, "WebAssembly.Component");
    return nullptr;
  }

  RootedObject proto(cx, &cx->global()->getPrototype(JSProto_WasmComponent));
  return WasmComponentObject::create(cx, *this, proto);
}

bool Component::addImport(ComponentImport&& import) {
  ComponentSort sort = import.externDesc().sort();
  MOZ_ASSERT(ComponentSortValidForExternDesc(sort));

  uint32_t importIndex = imports_.length();
  if (!imports_.append(std::move(import))) {
    return false;
  }

  return appendToExternIndexSpace(sort,
                                  ComponentItem::import(sort, importIndex));
}

bool Component::addExport(ComponentExport&& exp) {
  ComponentSort sort = exp.externDesc().sort();
  MOZ_ASSERT(ComponentSortValidForExternDesc(sort));

  uint32_t exportIndex = exports_.length();
  if (!exports_.append(std::move(exp))) {
    return false;
  }

  return appendToExternIndexSpace(sort,
                                  ComponentItem::export_(sort, exportIndex));
}

ComponentType Component::getType(uint32_t typeIndex) const {
  ComponentItem item = types_[typeIndex];
  MOZ_ASSERT(item.sort() == ComponentSort::Type);
  switch (item.kind()) {
    case ComponentItem::ItemKind::Defined:
      return definedTypes_[item.itemIndex()];
    case ComponentItem::ItemKind::Import:
      return imports_[item.itemIndex()].externDesc().asType();
    case ComponentItem::ItemKind::Export:
      return exports_[item.itemIndex()].externDesc().asType();
    case ComponentItem::ItemKind::Alias:
      MOZ_CRASH("should be impossible for now");
    default:
      MOZ_CRASH();
  }
}

ComponentType Component::getTypeForFunc(uint32_t funcIndex) const {
  ComponentItem item = funcs_[funcIndex];
  MOZ_ASSERT(item.sort() == ComponentSort::Func);
  switch (item.kind()) {
    case ComponentItem::ItemKind::Defined:
      return getType(definedFuncs_[item.itemIndex()].typeIndex());
    case ComponentItem::ItemKind::Import:
      return imports_[item.itemIndex()].externDesc().asFunc();
    case ComponentItem::ItemKind::Export:
      return exports_[item.itemIndex()].externDesc().asFunc();
    case ComponentItem::ItemKind::Alias:
      MOZ_CRASH("should be impossible for now");
    default:
      MOZ_CRASH();
  }
}

const TypeDef& Component::getTypeForCoreFunc(uint32_t coreFuncIndex) const {
  ComponentItem item = coreFuncs_[coreFuncIndex];
  MOZ_ASSERT(item.sort() == ComponentSort::CoreFunction);
  switch (item.kind()) {
    case ComponentItem::ItemKind::Defined:
      return *definedCoreFuncs_[item.itemIndex()].coreFuncType();
    case ComponentItem::ItemKind::Import:
    case ComponentItem::ItemKind::Export:
      // Core funcs cannot be imported or exported.
      MOZ_CRASH();
    case ComponentItem::ItemKind::Alias: {
      if (item.aliasKind() == ComponentAliasKind::Outer) {
        // TODO(wasm-cm): Right now we should only produce outer aliases with
        // an index of 0. In the future this will need to be expanded to
        // handle all outer aliases.
        //
        // TODO(wasm-cm): Well, but is it ever valid to outer-alias a core
        // thing (other than a module) with a depth other than 0? Probably
        // not.
        MOZ_RELEASE_ASSERT(item.aliasInstanceIndex() == 0);
        return getTypeForCoreFunc(item.itemIndex());
      }
      MOZ_RELEASE_ASSERT(item.aliasKind() == ComponentAliasKind::CoreExport);
      return getCoreInstance(item.aliasInstanceIndex())
          .getCoreFuncType(item.itemIndex());
    } break;
    default:
      MOZ_CRASH();
  }
}

const ComponentLiftedFuncDesc& Component::getLiftedFunc(
    uint32_t funcIndex) const {
  ComponentItem item = funcs_[funcIndex];
  MOZ_ASSERT(item.sort() == ComponentSort::Func);
  MOZ_RELEASE_ASSERT(item.kind() == ComponentItem::ItemKind::Defined);
  return definedFuncs_[item.itemIndex()];
}

const ComponentCoreFuncDesc& Component::getLoweredCoreFunc(
    uint32_t coreFuncIndex) const {
  ComponentItem item = coreFuncs_[coreFuncIndex];
  MOZ_ASSERT(item.sort() == ComponentSort::CoreFunction);
  MOZ_RELEASE_ASSERT(item.kind() == ComponentItem::ItemKind::Defined);
  return definedCoreFuncs_[item.itemIndex()];
}

const TableDesc& Component::getCoreTable(uint32_t tableIndex) const {
  ComponentItem item = coreTables_[tableIndex];
  MOZ_ASSERT(item.sort() == ComponentSort::CoreTable);
  switch (item.kind()) {
    case ComponentItem::ItemKind::Defined:
    case ComponentItem::ItemKind::Import:
    case ComponentItem::ItemKind::Export:
      // Tables cannot be defined, imported, or exported.
      MOZ_CRASH();
    case ComponentItem::ItemKind::Alias: {
      if (item.aliasKind() == ComponentAliasKind::Outer) {
        // TODO(wasm-cm): Right now we should only produce outer aliases with
        // an index of 0. In the future this will need to be expanded to
        // handle all outer aliases.
        MOZ_RELEASE_ASSERT(item.aliasInstanceIndex() == 0);
        return getCoreTable(item.itemIndex());
      }
      MOZ_RELEASE_ASSERT(item.aliasKind() == ComponentAliasKind::CoreExport);

      return getCoreInstance(item.aliasInstanceIndex())
          .getTable(item.itemIndex());
    } break;
    default:
      MOZ_CRASH();
  }
}

const MemoryDesc& Component::getCoreMemory(uint32_t memoryIndex) const {
  ComponentItem item = coreMemories_[memoryIndex];
  MOZ_ASSERT(item.sort() == ComponentSort::CoreMemory);
  switch (item.kind()) {
    case ComponentItem::ItemKind::Defined:
    case ComponentItem::ItemKind::Import:
    case ComponentItem::ItemKind::Export:
      // Memories cannot be defined, imported, or exported.
      MOZ_CRASH();
    case ComponentItem::ItemKind::Alias: {
      if (item.aliasKind() == ComponentAliasKind::Outer) {
        // TODO(wasm-cm): Right now we should only produce outer aliases with
        // an index of 0. In the future this will need to be expanded to
        // handle all outer aliases.
        MOZ_RELEASE_ASSERT(item.aliasInstanceIndex() == 0);
        return getCoreMemory(item.itemIndex());
      }
      MOZ_RELEASE_ASSERT(item.aliasKind() == ComponentAliasKind::CoreExport);

      return getCoreInstance(item.aliasInstanceIndex())
          .getMemory(item.itemIndex());
    } break;
    default:
      MOZ_CRASH();
  }
}

const GlobalDesc& Component::getCoreGlobal(uint32_t globalIndex) const {
  ComponentItem item = coreGlobals_[globalIndex];
  MOZ_ASSERT(item.sort() == ComponentSort::CoreGlobal);
  switch (item.kind()) {
    case ComponentItem::ItemKind::Defined:
    case ComponentItem::ItemKind::Import:
    case ComponentItem::ItemKind::Export:
      // Globals cannot be defined, imported, or exported.
      MOZ_CRASH();
    case ComponentItem::ItemKind::Alias: {
      if (item.aliasKind() == ComponentAliasKind::Outer) {
        // TODO(wasm-cm): Right now we should only produce outer aliases with
        // an index of 0. In the future this will need to be expanded to
        // handle all outer aliases.
        MOZ_RELEASE_ASSERT(item.aliasInstanceIndex() == 0);
        return getCoreGlobal(item.itemIndex());
      }
      MOZ_RELEASE_ASSERT(item.aliasKind() == ComponentAliasKind::CoreExport);

      return getCoreInstance(item.aliasInstanceIndex())
          .getGlobal(item.itemIndex());
    } break;
    default:
      MOZ_CRASH();
  }
}

const TagDesc& Component::getCoreTag(uint32_t tagIndex) const {
  ComponentItem item = coreTags_[tagIndex];
  MOZ_ASSERT(item.sort() == ComponentSort::CoreTag);
  switch (item.kind()) {
    case ComponentItem::ItemKind::Defined:
    case ComponentItem::ItemKind::Import:
    case ComponentItem::ItemKind::Export:
      // Tags cannot be defined, imported, or exported.
      MOZ_CRASH();
    case ComponentItem::ItemKind::Alias: {
      if (item.aliasKind() == ComponentAliasKind::Outer) {
        // TODO(wasm-cm): Right now we should only produce outer aliases with
        // an index of 0. In the future this will need to be expanded to
        // handle all outer aliases.
        MOZ_RELEASE_ASSERT(item.aliasInstanceIndex() == 0);
        return getCoreTag(item.itemIndex());
      }
      MOZ_RELEASE_ASSERT(item.aliasKind() == ComponentAliasKind::CoreExport);

      return getCoreInstance(item.aliasInstanceIndex())
          .getTag(item.itemIndex());
    } break;
    default:
      MOZ_CRASH();
  }
}

SharedModule Component::getCoreModule(uint32_t modIndex) const {
  ComponentItem item = coreModules_[modIndex];
  MOZ_ASSERT(item.sort() == ComponentSort::CoreModule);
  switch (item.kind()) {
    case ComponentItem::ItemKind::Defined:
      return definedCoreModules_[item.itemIndex()];
    case ComponentItem::ItemKind::Import:
      // TODO(wasm-cm): Fix when core module types are supported
      MOZ_CRASH("should be impossible for now");
    case ComponentItem::ItemKind::Export: {
      const ComponentExport& exp = exports_[item.itemIndex()];
      MOZ_ASSERT(exp.externDesc().sort() == ComponentSort::CoreModule);
      return definedCoreModules_[exp.externDesc().asCoreModule()];
    } break;
    case ComponentItem::ItemKind::Alias:
      // TODO(wasm-cm): Fix when nested components are supported
      MOZ_CRASH("should be impossible for now");
    default:
      MOZ_CRASH();
  }
}

const ModuleMetadata& Component::getCoreModuleMetadata(
    uint32_t modIndex) const {
  ComponentItem item = coreModules_[modIndex];
  MOZ_ASSERT(item.sort() == ComponentSort::CoreModule);
  switch (item.kind()) {
    case ComponentItem::ItemKind::Defined:
      return *definedCoreModuleMeta_[item.itemIndex()];
    case ComponentItem::ItemKind::Import:
      // TODO(wasm-cm): Fix when core module types are supported
      MOZ_CRASH("should be impossible for now");
    case ComponentItem::ItemKind::Export: {
      const ComponentExport& exp = exports_[item.itemIndex()];
      MOZ_ASSERT(exp.externDesc().sort() == ComponentSort::CoreModule);
      return *definedCoreModuleMeta_[exp.externDesc().asCoreModule()];
    } break;
    case ComponentItem::ItemKind::Alias:
      // TODO(wasm-cm): Fix when nested components are supported
      MOZ_CRASH("should be impossible for now");
    default:
      MOZ_CRASH();
  }
}

const CoreInstanceDesc& Component::getCoreInstance(
    uint32_t instanceIndex) const {
  ComponentItem item = coreInstances_[instanceIndex];
  MOZ_ASSERT(item.sort() == ComponentSort::CoreInstance);
  switch (item.kind()) {
    case ComponentItem::ItemKind::Defined:
      return definedCoreInstances_[item.itemIndex()];
    case ComponentItem::ItemKind::Import:
    case ComponentItem::ItemKind::Export:
    case ComponentItem::ItemKind::Alias:
      // Core instances cannot be imported or exported, so they cannot be
      // aliased either.
      MOZ_CRASH();
    default:
      MOZ_CRASH();
  }
}

ComponentItem Component::resolveSortIndex(ComponentSortIndex sortIndex) const {
  switch (sortIndex.sort) {
    case ComponentSort::Func:
      return funcs_[sortIndex.index];
    case ComponentSort::Type:
      return types_[sortIndex.index];
    case ComponentSort::Component:
    case ComponentSort::Instance:
      // TODO(wasm-cm): Nested components
      MOZ_CRASH();

    case ComponentSort::CoreFunction:
      return coreFuncs_[sortIndex.index];
    case ComponentSort::CoreTable:
      return coreTables_[sortIndex.index];
    case ComponentSort::CoreMemory:
      return coreMemories_[sortIndex.index];
    case ComponentSort::CoreGlobal:
      return coreGlobals_[sortIndex.index];
    case ComponentSort::CoreTag:
      return coreTags_[sortIndex.index];

    case ComponentSort::CoreType:
      return coreTypes_[sortIndex.index];
    case ComponentSort::CoreModule:
      return coreModules_[sortIndex.index];
    case ComponentSort::CoreInstance:
      return coreInstances_[sortIndex.index];

    default:
      MOZ_CRASH();
  }
}

bool Component::saveExportNameForAlias(ComponentSortIndex sortIndexOfAlias,
                                       CacheableName&& name) {
  ComponentItem alias = resolveSortIndex(sortIndexOfAlias);
  MOZ_RELEASE_ASSERT(alias.kind() == ComponentItem::ItemKind::Alias);
  MOZ_RELEASE_ASSERT(alias.aliasKind() == ComponentAliasKind::CoreExport ||
                     alias.aliasKind() == ComponentAliasKind::Export);

  auto p = aliasNames_.lookupForAdd(sortIndexOfAlias);
  MOZ_RELEASE_ASSERT(!p);
  return aliasNames_.add(p, sortIndexOfAlias, std::move(name));
}

const CacheableName& Component::getExportNameForAlias(
    ComponentSortIndex sortIndexOfAlias) const {
  auto p = aliasNames_.lookup(sortIndexOfAlias);
  MOZ_RELEASE_ASSERT(p);
  return p->value();
}

bool Component::tryMarkResourceInterfaceBound(uint32_t typeIndex,
                                              bool* alreadyBound) const {
  auto addPtr = resourceInterfaceBoundTypeIndices_.lookupForAdd(typeIndex);
  if (addPtr) {
    *alreadyBound = true;
    return true;
  }
  *alreadyBound = false;
  return resourceInterfaceBoundTypeIndices_.add(addPtr, typeIndex);
}

bool Component::instantiate(
    JSContext* cx, HandleObject instanceProto, HandleObject importObj,
    MutableHandle<WasmComponentInstanceObject*> instance) const {
  instance.set(
      WasmComponentInstanceObject::create(cx, instanceProto, this, importObj));
  if (!instance) {
    return false;
  }

  return true;
}

ComponentInstance::ComponentInstance(
    JSContext* cx, Handle<WasmComponentInstanceObject*> object,
    const SharedComponent component)
    : realm_(cx->realm()),
      cx_(cx),
      component_(component),
      object_(object),
      coreInstances_(cx->zone()),
      componentFuncs_(cx->zone()) {}

ComponentInstance::~ComponentInstance() = default;

ComponentInstance* ComponentInstance::create(
    JSContext* cx, Handle<WasmComponentInstanceObject*> object,
    const SharedComponent component) {
  ComponentInstance* instance =
      js_new<ComponentInstance>(cx, object, component);
  if (!instance) {
    ReportOutOfMemory(cx);
    return nullptr;
  }
  return instance;
}

void ComponentInstance::destroy(ComponentInstance* instance) {
  instance->~ComponentInstance();
  js_free(instance);
}

void ComponentInstance::tracePrivate(JSTracer* trc) {
  TraceEdge(trc, &object_, "wasm component instance object");
  coreInstances_.trace(trc);
  componentFuncs_.trace(trc);
  for (const mozilla::UniquePtr<LoweredNativeClosure>& closure :
       loweredNativeClosures_) {
    if (closure) {
      closure->trace(trc);
    }
  }
  handles_.trace(trc);
}

// Expected to be infallible here due to previous validation.
[[nodiscard]] static bool GetValidatedNamedPropertyFromObject(
    JSContext* cx, HandleObject obj, const CacheableName& name,
    MutableHandleValue exportVal) {
  JSAtom* atom = name.toAtom(cx);
  if (!atom) {
    return false;
  }
  RootedId id(cx, AtomToId(atom));
  MOZ_RELEASE_ASSERT(JS_GetPropertyById(cx, obj, id, exportVal));
  return true;
}

// Resolves a sort/index pair through at most one level of outer alias (deeper
// chains aren't modeled yet) to a CoreExport alias, then reads the named
// export off the instantiated core instance.
bool ComponentInstance::resolveCoreExport(ComponentSort sort, uint32_t index,
                                          MutableHandleValue out) const {
  ComponentSortIndex sortIndex(sort, index);
  ComponentItem item = component_->resolveSortIndex(sortIndex);
  if (item.isOuterAlias()) {
    MOZ_RELEASE_ASSERT(item.aliasInstanceIndex() == 0);
    sortIndex = item.outerAliasSortIndex();
    item = component_->resolveSortIndex(sortIndex);
  }
  MOZ_RELEASE_ASSERT(item.kind() == ComponentItem::ItemKind::Alias);
  MOZ_RELEASE_ASSERT(item.aliasKind() == ComponentAliasKind::CoreExport);

  WasmInstanceObject* srcInstance = coreInstance(item.aliasInstanceIndex());
  MOZ_RELEASE_ASSERT(srcInstance);
  RootedObject exportsObj(cx_, &srcInstance->exportsObj());
  const CacheableName& exportName =
      component_->getExportNameForAlias(sortIndex);
  return GetValidatedNamedPropertyFromObject(cx_, exportsObj, exportName, out);
}

bool ComponentInstance::resolveCoreMemory(
    uint32_t memIndex, MutableHandle<WasmMemoryObject*> out) const {
  RootedValue exportVal(cx_);
  if (!resolveCoreExport(ComponentSort::CoreMemory, memIndex, &exportVal)) {
    return false;
  }
  out.set(&exportVal.toObject().as<WasmMemoryObject>());
  return true;
}

bool ComponentInstance::resolveCoreFunc(uint32_t coreFuncIndex,
                                        MutableHandleObject out) const {
  RootedValue exportVal(cx_);
  if (!resolveCoreExport(ComponentSort::CoreFunction, coreFuncIndex,
                         &exportVal)) {
    return false;
  }
  out.set(&exportVal.toObject());
  return true;
}

[[nodiscard]] static bool GenerateDummyLoweredFunc(
    JSContext* cx, SharedTypeDef funcTypeDef, MutableHandleValue funcObject) {
  SharedCompileArgs compileArgs = CompileArgs::buildAndReport(
      cx, ScriptedCaller::selfHosted(cx), FeatureOptions());
  if (!compileArgs) {
    return false;
  }

  MutableModuleMetadata moduleMeta = js_new<ModuleMetadata>();
  if (!moduleMeta || !moduleMeta->init(*compileArgs)) {
    ReportOutOfMemory(cx);
    return false;
  }
  MutableCodeMetadata codeMeta = moduleMeta->codeMeta;

  CompilerEnvironment compilerEnv(
      CompileMode::Once,
      BaselineAvailable(cx) ? Tier::Baseline : Tier::Optimized,
      DebugEnabled::False);
  compilerEnv.computeParameters();

  MOZ_RELEASE_ASSERT(funcTypeDef->recGroup().numTypes() == 1);
  if (!codeMeta->types->addRecGroup(&funcTypeDef->recGroup())) {
    ReportOutOfMemory(cx);
    return false;
  }
  if (!moduleMeta->addDefinedFuncWithType(
          /*funcTypeIndex=*/0,
          /*declareForRef=*/true,
          /*optionalExportedName=*/mozilla::Some(CacheableName()))) {
    ReportOutOfMemory(cx);
    return false;
  }

  if (!moduleMeta->prepareForCompile(compilerEnv.mode())) {
    ReportOutOfMemory(cx);
    return false;
  }

  ModuleGenerator mg(*codeMeta, compilerEnv, compilerEnv.initialState(),
                     nullptr, nullptr, nullptr);
  if (!mg.initializeCompleteTier()) {
    ReportOutOfMemory(cx);
    return false;
  }

  Bytes bytecode;
  Encoder encoder(bytecode, *codeMeta->types);
  if (!encoder.writeVarU32(0)) {  // locals
    return false;
  }
  if (!encoder.writeOp(Op::Unreachable) || !encoder.writeOp(Op::End)) {
    ReportOutOfMemory(cx);
    return false;
  }
  if (!mg.compileFuncDef(/*funcIndex=*/0, CallSite::FIRST_VALID_BYTECODE_OFFSET,
                         bytecode.begin(),
                         bytecode.begin() + bytecode.length())) {
    ReportOutOfMemory(cx);
    return false;
  }

  if (!mg.finishFuncDefs()) {
    ReportOutOfMemory(cx);
    return false;
  }

  SharedModule m = mg.finishModule(BytecodeBufferOrSource(), *moduleMeta,
                                   /*maybeCompleteTier2Listener=*/nullptr);
  if (!m) {
    ReportOutOfMemory(cx);
    return false;
  }

  Rooted<WasmInstanceObject*> instanceObj(cx);
  ImportValues imports;
  if (!m->instantiate(cx, imports, nullptr, &instanceObj)) {
    return false;
  }
  RootedFunction exportedFunc(cx);
  if (!instanceObj->instance().getExportedFunction(cx, /*funcIndex=*/0,
                                                   &exportedFunc)) {
    return false;
  }
  funcObject.setObject(*exportedFunc);

  return true;
}

// JIT-compiles a wasm stub with `funcTypeDef`'s shape whose entire body is a
// single CallCanonLowerNative through `closure` (already fully populated by
// the caller) - the native-call counterpart to GenerateDummyLoweredFunc
// above. Always compiles at Tier::Baseline, since that's the only tier
// CallCanonLowerNative is implemented for; if baseline isn't available at
// all on this platform, `*baselineUnavailable` comes back true and the
// caller should fall back to a WasmComponentLoweredJS closure instead.
[[nodiscard]] static bool GenerateCanonLowerNative(
    JSContext* cx, SharedTypeDef funcTypeDef, LoweredNativeClosure* closure,
    bool* baselineUnavailable, MutableHandleValue funcObject) {
  *baselineUnavailable = false;
  // Platform capability only, not BaselineAvailable(cx): this stub is
  // internal glue with no Ion counterpart, not user wasm code subject to
  // the embedder's/shell's tier-selection preference (e.g. the jit-test
  // `--wasm-compiler=optimizing` matrix variant, which disables baseline
  // for user modules but has nothing to compare this stub's single tier
  // against).
  if (!BaselinePlatformSupport()) {
    *baselineUnavailable = true;
    return true;
  }

  UniqueChars name = JS_smprintf("canon.lower");
  if (!name) {
    return false;
  }
  ScriptedCaller scriptedCaller(std::move(name), ScriptedCallerKind::Url, 0);

  FeatureOptions featureOptions;
  featureOptions.isBuiltinModule = true;
  SharedCompileArgs compileArgs = CompileArgs::buildAndReport(
      cx, std::move(scriptedCaller), featureOptions);
  if (!compileArgs) {
    return false;
  }

  MutableModuleMetadata moduleMeta = js_new<ModuleMetadata>();
  if (!moduleMeta || !moduleMeta->init(*compileArgs)) {
    ReportOutOfMemory(cx);
    return false;
  }
  MutableCodeMetadata codeMeta = moduleMeta->codeMeta;

  CompilerEnvironment compilerEnv(CompileMode::Once, Tier::Baseline,
                                  DebugEnabled::False);
  compilerEnv.computeParameters();

  MOZ_RELEASE_ASSERT(funcTypeDef->recGroup().numTypes() == 1);
  if (!codeMeta->types->addRecGroup(&funcTypeDef->recGroup())) {
    ReportOutOfMemory(cx);
    return false;
  }
  if (!moduleMeta->addDefinedFuncWithType(
          /*funcTypeIndex=*/0,
          /*declareForRef=*/true,
          /*optionalExportedName=*/mozilla::Some(CacheableName()))) {
    ReportOutOfMemory(cx);
    return false;
  }

  if (!moduleMeta->prepareForCompile(compilerEnv.mode())) {
    ReportOutOfMemory(cx);
    return false;
  }

  ModuleGenerator mg(*codeMeta, compilerEnv, compilerEnv.initialState(),
                     nullptr, nullptr, nullptr);
  if (!mg.initializeCompleteTier()) {
    ReportOutOfMemory(cx);
    return false;
  }

  const FuncType& funcType = funcTypeDef->funcType();
  Bytes bytecode;
  Encoder encoder(bytecode, *codeMeta->types);
  if (!encoder.writeVarU32(0)) {  // locals
    return false;
  }
  for (uint32_t i = 0; i < funcType.args().length(); i++) {
    if (!encoder.writeOp(Op::LocalGet) || !encoder.writeVarU32(i)) {
      ReportOutOfMemory(cx);
      return false;
    }
  }
  if (!encoder.writeOp(MozOp::CallCanonLowerNative) ||
      !encoder.writeVarU64(uint64_t(uintptr_t(closure)))) {
    ReportOutOfMemory(cx);
    return false;
  }
  if (!encoder.writeOp(Op::End)) {
    ReportOutOfMemory(cx);
    return false;
  }

  if (!mg.compileFuncDef(/*funcIndex=*/0, CallSite::FIRST_VALID_BYTECODE_OFFSET,
                         bytecode.begin(),
                         bytecode.begin() + bytecode.length())) {
    ReportOutOfMemory(cx);
    return false;
  }

  if (!mg.finishFuncDefs()) {
    ReportOutOfMemory(cx);
    return false;
  }

  SharedModule m = mg.finishModule(BytecodeBufferOrSource(), *moduleMeta,
                                   /*maybeCompleteTier2Listener=*/nullptr);
  if (!m) {
    ReportOutOfMemory(cx);
    return false;
  }

  Rooted<WasmInstanceObject*> instanceObj(cx);
  ImportValues imports;
  if (!m->instantiate(cx, imports, nullptr, &instanceObj)) {
    return false;
  }
  RootedFunction exportedFunc(cx);
  if (!instanceObj->instance().getExportedFunction(cx, /*funcIndex=*/0,
                                                   &exportedFunc)) {
    return false;
  }
  funcObject.setObject(*exportedFunc);

  return true;
}

// Canon lift becomes a JS-callable export closure, canon lower a JS-callable
// core import closure, each resolving callee/memory/realloc/postReturn
// lazily from the instance at call time.

namespace {
enum ComponentLiftedEntryStateSlot {
  ComponentLiftedEntryState_InstanceObj,
  ComponentLiftedEntryState_FuncIndex,
  ComponentLiftedEntryState_SlotCount,
};
}  // namespace

static const JSClass ComponentLiftedEntryStateClass = {
    "ComponentLiftedEntryState",
    JSCLASS_HAS_RESERVED_SLOTS(ComponentLiftedEntryState_SlotCount)};

static bool WasmComponentLiftedEntry(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  RootedObject state(cx,
                     &GetFunctionNativeReserved(&args.callee(), 0).toObject());
  Rooted<WasmComponentInstanceObject*> instanceObj(
      cx, &JS::GetReservedSlot(state, ComponentLiftedEntryState_InstanceObj)
               .toObject()
               .as<WasmComponentInstanceObject>());
  uint32_t funcIndex =
      uint32_t(JS::GetReservedSlot(state, ComponentLiftedEntryState_FuncIndex)
                   .toInt32());

  ComponentInstance& instance = instanceObj->instance();
  const ComponentLiftedFuncDesc& desc =
      instance.component()->getLiftedFunc(funcIndex);
  ComponentType funcTypeVal = instance.component()->getTypeForFunc(funcIndex);
  const ComponentFuncType& funcType = funcTypeVal.asFunc();
  const ComponentCanonOpts& opts = desc.canonOpts();

  RootedObject coreCallee(cx);
  if (!instance.resolveCoreFunc(desc.coreFuncIndex(), &coreCallee)) {
    return false;
  }

  Rooted<WasmMemoryObject*> memory(cx);
  if (opts.memoryIndex) {
    if (!instance.resolveCoreMemory(*opts.memoryIndex, &memory)) {
      return false;
    }
  }
  RootedObject realloc(cx);
  if (opts.reallocIndex) {
    if (!instance.resolveCoreFunc(*opts.reallocIndex, &realloc)) {
      return false;
    }
  }
  RootedValue postReturn(cx);
  if (opts.postReturnIndex) {
    RootedObject postReturnFunc(cx);
    if (!instance.resolveCoreFunc(*opts.postReturnIndex, &postReturnFunc)) {
      return false;
    }
    postReturn.setObject(*postReturnFunc);
  }

  uint8_t* memoryBase = nullptr;
  if (memory) {
    MOZ_RELEASE_ASSERT(!memory->isShared() && !memory->movingGrowable());
    memoryBase = memory->buffer().dataPointerEither().unwrap();
  }
  JS::WebIDLContext abiCtx(cx, memory, memoryBase, realloc,
                           &instance.handles());
  return LiftedCall(cx, coreCallee, funcType, abiCtx, postReturn, args);
}

static JSFunction* BuildLiftedExport(
    JSContext* cx, Handle<WasmComponentInstanceObject*> instanceObj,
    uint32_t funcIndex) {
  RootedObject state(cx, JS_NewObject(cx, &ComponentLiftedEntryStateClass));
  if (!state) {
    return nullptr;
  }
  JS::SetReservedSlot(state, ComponentLiftedEntryState_InstanceObj,
                      ObjectValue(*instanceObj));
  JS::SetReservedSlot(state, ComponentLiftedEntryState_FuncIndex,
                      JS::Int32Value(int32_t(funcIndex)));

  RootedFunction fun(
      cx, NewFunctionWithReserved(cx, WasmComponentLiftedEntry, 0, 0, nullptr));
  if (!fun) {
    return nullptr;
  }
  SetFunctionNativeReserved(fun, 0, ObjectValue(*state));
  return fun;
}

namespace {
enum ComponentLoweredEntryStateSlot {
  ComponentLoweredEntryState_InstanceObj,
  ComponentLoweredEntryState_CoreFuncIndex,
  ComponentLoweredEntryState_SlotCount,
};
}  // namespace

static const JSClass ComponentLoweredEntryStateClass = {
    "ComponentLoweredEntryState",
    JSCLASS_HAS_RESERVED_SLOTS(ComponentLoweredEntryState_SlotCount)};

// Best-effort import name for a component func index, for MOZ_LOG=
// wasmComponents:3 diagnostics only - e.g. correlating a "statically doomed
// binding"/"component call trap" error with the specific import that
// produced it. Returns Nothing for a non-import func (canon lift, re-export)
// or an out-of-range index.
//
// [[maybe_unused]]: its only caller is currently commented out below.
[[maybe_unused]] static mozilla::Maybe<mozilla::Span<const char>>
ComponentFuncImportNameForLog(const Component& component, uint32_t funcIndex) {
  if (funcIndex >= component.funcs().length()) {
    return mozilla::Nothing();
  }
  ComponentItem item = component.funcs()[funcIndex];
  if (item.kind() != ComponentItem::ItemKind::Import) {
    return mozilla::Nothing();
  }
  return mozilla::Some(
      component.imports()[item.itemIndex()].name().utf8Bytes());
}

// Defined alongside their other call sites, further below.
static bool RawCoreValuesToVals(JSContext* cx, JS::HandleValueArray rawArgs,
                                const ComponentTypeVector& types,
                                uint32_t maxFlat, RootedValVector& out);
static bool AppendScalarValsAsVals(
    mozilla::Span<const JS::WasmScalarVal> scalars,
    mozilla::Span<const ValType> types, RootedValVector* out);

// JS::WebIDLNativeCall's boundary uses JS::WasmScalarVal, not wasm::Val;
// converts where a wasm::Val is still genuinely needed on this side.
static JS::WasmScalarVal ValToScalarVal(const Val& v) {
  switch (v.type().kind()) {
    case ValType::Kind::I32:
      return JS::WasmScalarVal::fromI32(v.i32());
    case ValType::Kind::I64:
      return JS::WasmScalarVal::fromI64(v.i64());
    case ValType::Kind::F32:
      return JS::WasmScalarVal::fromF32(v.f32());
    case ValType::Kind::F64:
      return JS::WasmScalarVal::fromF64(v.f64());
    default:
      MOZ_CRASH(
          "non-scalar core value crossing the WebIDL native call boundary");
  }
}

// A thin JS-value bridge over the closure's own JS::WebIDLNativeCall entry -
// every `canon lower` core func has a LoweredNativeClosure by the time this
// runs (see ResolveLoweredCoreFunc), whether or not its callee turned out to
// have a native overload; this is only reached from a call this instance's
// core code couldn't inline (e.g. call_indirect) or, per
// GenerateCanonLowerNative, on a platform with no baseline compiler.
static bool WasmComponentLoweredJS(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  RootedObject state(cx,
                     &GetFunctionNativeReserved(&args.callee(), 0).toObject());
  Rooted<WasmComponentInstanceObject*> instanceObj(
      cx, &JS::GetReservedSlot(state, ComponentLoweredEntryState_InstanceObj)
               .toObject()
               .as<WasmComponentInstanceObject>());
  uint32_t coreFuncIndex = uint32_t(
      JS::GetReservedSlot(state, ComponentLoweredEntryState_CoreFuncIndex)
          .toInt32());

  ComponentInstance& instance = instanceObj->instance();
  LoweredNativeClosure* closure = instance.loweredNativeClosure(coreFuncIndex);
  MOZ_ASSERT(closure);

  ComponentType funcTypeVal =
      instance.component()->getTypeForFunc(closure->loweredFuncIndex);
  const ComponentFuncType& funcType = funcTypeVal.asFunc();

  RootedValVector coreParams(cx);
  if (!RawCoreValuesToVals(cx, JS::HandleValueArray(args), funcType.paramTypes,
                           MaxFlatParams, coreParams)) {
    return false;
  }
  MOZ_ASSERT(coreParams.length() == closure->numParams);
  for (uint32_t i = 0; i < closure->numParams; i++) {
    closure->paramsScratch[i] = ValToScalarVal(coreParams[i].get());
  }

  uint32_t resultMemBase = 0;
  if (closure->resultOutParamIndex >= 0) {
    RootedVal v(cx);
    if (!Val::fromJSValue(cx, ValType::i32(), args[args.length() - 1], &v)) {
      return false;
    }
    resultMemBase = v.get().i32();
  }

  if (!closure->entry(&closure->adapter, &closure->ctx,
                      closure->paramsScratch.get(), resultMemBase,
                      closure->resultsScratch.get())) {
    return false;
  }

  if (closure->numResultSlots == 0) {
    args.rval().setUndefined();
    return true;
  }
  ValTypeVector flatResultTypes;
  if (!FlattenType(funcType.resultType.ref(), &flatResultTypes)) {
    return false;
  }
  RootedValVector coreResults(cx);
  mozilla::Span<const JS::WasmScalarVal> resultSlots(
      closure->resultsScratch.get(), closure->numResultSlots);
  mozilla::Span<const ValType> resultTypes(flatResultTypes.begin(),
                                           closure->numResultSlots);
  if (!AppendScalarValsAsVals(resultSlots, resultTypes, &coreResults)) {
    return false;
  }
  return coreResults[0].get().toJSValue(cx, args.rval());
}

static JSFunction* BuildLoweredJSCoreFunc(
    JSContext* cx, Handle<WasmComponentInstanceObject*> instanceObj,
    uint32_t coreFuncIndex) {
  RootedObject state(cx, JS_NewObject(cx, &ComponentLoweredEntryStateClass));
  if (!state) {
    return nullptr;
  }
  JS::SetReservedSlot(state, ComponentLoweredEntryState_InstanceObj,
                      ObjectValue(*instanceObj));
  JS::SetReservedSlot(state, ComponentLoweredEntryState_CoreFuncIndex,
                      JS::Int32Value(int32_t(coreFuncIndex)));

  RootedFunction fun(
      cx, NewFunctionWithReserved(cx, WasmComponentLoweredJS, 0, 0, nullptr));
  if (!fun) {
    return nullptr;
  }
  SetFunctionNativeReserved(fun, 0, ObjectValue(*state));
  return fun;
}

// Defined alongside TryCallWebIDLNative's former call site, further below.
[[nodiscard]] static bool SelectNativeOverload(
    JSContext* cx, JS::HandleFunction callee, const ComponentFuncType& funcType,
    bool* eligible, JS::WebIDLNativeCall* outEntry, CallAdapter* outAdapter);

// The generic JS::WebIDLNativeCall entry - defined alongside its own former
// call site (WasmComponentLoweredJS's predecessor), further below.
static bool LoweredNativeCall(const JS::WebIDLCallAdapter* adapter,
                              JS::WebIDLContext* ctx,
                              const JS::WasmScalarVal* params,
                              uint32_t resultMemBase,
                              JS::WasmScalarVal* results);

// Resolves a `canon lower` core func's import into a LoweredNativeClosure and
// stores its JSFunction form in `exportVal`. Called from
// ComponentInstance::init, once per lowered core func, instead of redoing
// this decision on every call.
//
// Every core func gets a closure, whether or not its callee has a usable
// native overload (see SelectNativeOverload): an eligible callee's closure
// calls straight into the selected overload; an ineligible one's calls
// LoweredNativeCall, the generic entry that drives an arbitrary JS callee
// through the same adapter machinery a native overload would.
[[nodiscard]] static bool ResolveLoweredCoreFunc(
    JSContext* cx, Handle<WasmComponentInstanceObject*> instanceObj,
    uint32_t coreFuncIndex, MutableHandleValue exportVal) {
  ComponentInstance& instance = instanceObj->instance();
  const ComponentCoreFuncDesc& desc =
      instance.component()->getLoweredCoreFunc(coreFuncIndex);
  uint32_t loweredFuncIndex = desc.payload().as<uint32_t>();

  RootedObject callee(cx, instance.componentFunc(loweredFuncIndex));
  ComponentType funcTypeVal =
      instance.component()->getTypeForFunc(loweredFuncIndex);
  const ComponentFuncType& funcType = funcTypeVal.asFunc();

  // Logged before the decision so the "native call path: ..." line that
  // SelectNativeOverload emits next can be attributed to an import by name -
  // otherwise a page with several imports produces an unattributable list.
  if (mozilla::Maybe<mozilla::Span<const char>> importName =
          ComponentFuncImportNameForLog(*instance.component(),
                                        loweredFuncIndex)) {
    JS_LOG(wasmComponents, Info, "canon lower resolve: import %.*s",
           int(importName->Length()), importName->Elements());
  }

  bool eligible = false;
  JS::WebIDLNativeCall entry = nullptr;
  CallAdapter adapter;
  JSObject* unwrappedCallee = UncheckedUnwrap(callee);
  if (unwrappedCallee->is<JSFunction>()) {
    RootedFunction calleeFun(cx, &unwrappedCallee->as<JSFunction>());
    if (!SelectNativeOverload(cx, calleeFun, funcType, &eligible, &entry,
                              &adapter)) {
      return false;
    }
  }

  mozilla::UniquePtr<LoweredNativeClosure> closure =
      mozilla::MakeUnique<LoweredNativeClosure>();
  if (!closure) {
    ReportOutOfMemory(cx);
    return false;
  }
  closure->ctx.init(cx, &instance.handles());
  if (eligible) {
    closure->entry = entry;
    closure->adapter = std::move(adapter);
  } else {
    closure->entry = LoweredNativeCall;
    closure->instance = &instance;
    closure->loweredFuncIndex = loweredFuncIndex;
    // `callee` (an arbitrary component func: an import, or another lift) has
    // no WebIDL type of its own here, so use the "read the component
    // function import" fallback of a variadic `any` operation type.
    JS::WebIDLOperationType opType;
    if (!DefaultWebIDLOperationType(cx, funcType.resultType.isSome(),
                                    &opType) ||
        !BuildCallAdapter(cx, funcType, opType, ComponentStringEncoding::Utf8,
                          &closure->adapter)) {
      return false;
    }
  }

  bool hasResultOutParam = false;
  if (funcType.resultType.isSome()) {
    ValTypeVector flatResult;
    if (!FlattenType(funcType.resultType.ref(), &flatResult)) {
      return false;
    }
    hasResultOutParam = flatResult.length() > MaxFlatResults;
  }
  const TypeDef& coreFuncTypeDef =
      instance.component()->getTypeForCoreFunc(coreFuncIndex);
  const FuncType& coreWasmFuncType = coreFuncTypeDef.funcType();
  uint32_t wasmNumParams = coreWasmFuncType.args().length();
  closure->numParams = hasResultOutParam ? wasmNumParams - 1 : wasmNumParams;
  closure->resultOutParamIndex =
      hasResultOutParam ? int32_t(wasmNumParams - 1) : -1;
  closure->numResultSlots =
      hasResultOutParam ? 0 : coreWasmFuncType.results().length();
  if (closure->numParams > 0) {
    closure->paramsScratch =
        mozilla::MakeUnique<JS::WasmScalarVal[]>(closure->numParams);
    if (!closure->paramsScratch) {
      ReportOutOfMemory(cx);
      return false;
    }
  }
  if (closure->numResultSlots > 0) {
    closure->resultsScratch =
        mozilla::MakeUnique<JS::WasmScalarVal[]>(closure->numResultSlots);
    if (!closure->resultsScratch) {
      ReportOutOfMemory(cx);
      return false;
    }
  }

  bool baselineUnavailable = false;
  if (!GenerateCanonLowerNative(cx, &coreFuncTypeDef, closure.get(),
                                &baselineUnavailable, exportVal)) {
    return false;
  }
  instance.setLoweredNativeClosure(coreFuncIndex, std::move(closure));
  if (!baselineUnavailable) {
    return true;
  }

  JSFunction* loweredFun =
      BuildLoweredJSCoreFunc(cx, instanceObj, coreFuncIndex);
  if (!loweredFun) {
    return false;
  }
  exportVal.setObject(*loweredFun);
  return true;
}

/* static */ int32_t Instance::componentResourceDrop(
    Instance* instance, int64_t rawComponentInstance, int32_t index) {
  JSContext* cx = instance->cx();
  auto* componentInstance =
      reinterpret_cast<ComponentInstance*>(uintptr_t(rawComponentInstance));
  if (!componentInstance->handles().drop(cx, uint32_t(index),
                                         /*requireOwn=*/false)) {
    return -1;
  }
  return 0;
}

// JIT-compiles a wasm stub with `funcTypeDef`'s shape (the imported
// resource's `resource.drop` signature: a single i32 handle index, no
// results) whose entire body is a `global.get` of `componentInstance`
// (baked in as an immutable i64 global, since CallBuiltinModuleFunc's only
// immediate is a func id, leaving no room for a closure pointer) followed
// by a single CallBuiltinModuleFunc to the ComponentResourceDrop builtin -
// the builtin-module-func counterpart to GenerateCanonLowerNative above.
// Always compiles at Tier::Baseline; if baseline isn't available at all on
// this platform, `*baselineUnavailable` comes back true and the caller
// should fall back to the WasmComponentResourceDropEntry JSNative instead.
[[nodiscard]] static bool GenerateResourceDropFunc(
    JSContext* cx, SharedTypeDef funcTypeDef,
    ComponentInstance* componentInstance, bool* baselineUnavailable,
    MutableHandleValue funcObject) {
  *baselineUnavailable = false;
  if (!BaselinePlatformSupport()) {
    *baselineUnavailable = true;
    return true;
  }

  UniqueChars name = JS_smprintf("resource.drop");
  if (!name) {
    return false;
  }
  ScriptedCaller scriptedCaller(std::move(name), ScriptedCallerKind::Url, 0);

  FeatureOptions featureOptions;
  featureOptions.isBuiltinModule = true;
  SharedCompileArgs compileArgs = CompileArgs::buildAndReport(
      cx, std::move(scriptedCaller), featureOptions);
  if (!compileArgs) {
    return false;
  }

  MutableModuleMetadata moduleMeta = js_new<ModuleMetadata>();
  if (!moduleMeta || !moduleMeta->init(*compileArgs)) {
    ReportOutOfMemory(cx);
    return false;
  }
  MutableCodeMetadata codeMeta = moduleMeta->codeMeta;

  CompilerEnvironment compilerEnv(CompileMode::Once, Tier::Baseline,
                                  DebugEnabled::False);
  compilerEnv.computeParameters();

  MOZ_RELEASE_ASSERT(funcTypeDef->recGroup().numTypes() == 1);
  if (!codeMeta->types->addRecGroup(&funcTypeDef->recGroup())) {
    ReportOutOfMemory(cx);
    return false;
  }
  if (!moduleMeta->addDefinedFuncWithType(
          /*funcTypeIndex=*/0,
          /*declareForRef=*/true,
          /*optionalExportedName=*/mozilla::Some(CacheableName()))) {
    ReportOutOfMemory(cx);
    return false;
  }

  if (!codeMeta->globals.append(
          GlobalDesc(InitExpr(LitVal(uint64_t(uintptr_t(componentInstance)))),
                     /*isMutable=*/false))) {
    ReportOutOfMemory(cx);
    return false;
  }

  if (!moduleMeta->prepareForCompile(compilerEnv.mode())) {
    ReportOutOfMemory(cx);
    return false;
  }

  ModuleGenerator mg(*codeMeta, compilerEnv, compilerEnv.initialState(),
                     nullptr, nullptr, nullptr);
  if (!mg.initializeCompleteTier()) {
    ReportOutOfMemory(cx);
    return false;
  }

  const FuncType& funcType = funcTypeDef->funcType();
  Bytes bytecode;
  Encoder encoder(bytecode, *codeMeta->types);
  if (!encoder.writeVarU32(0)) {  // locals
    return false;
  }
  if (!encoder.writeOp(Op::GlobalGet) || !encoder.writeVarU32(0)) {
    ReportOutOfMemory(cx);
    return false;
  }
  for (uint32_t i = 0; i < funcType.args().length(); i++) {
    if (!encoder.writeOp(Op::LocalGet) || !encoder.writeVarU32(i)) {
      ReportOutOfMemory(cx);
      return false;
    }
  }
  if (!encoder.writeOp(MozOp::CallBuiltinModuleFunc) ||
      !encoder.writeVarU32(
          uint32_t(BuiltinModuleFuncId::ComponentResourceDrop))) {
    ReportOutOfMemory(cx);
    return false;
  }
  if (!encoder.writeOp(Op::End)) {
    ReportOutOfMemory(cx);
    return false;
  }

  if (!mg.compileFuncDef(/*funcIndex=*/0, CallSite::FIRST_VALID_BYTECODE_OFFSET,
                         bytecode.begin(),
                         bytecode.begin() + bytecode.length())) {
    ReportOutOfMemory(cx);
    return false;
  }

  if (!mg.finishFuncDefs()) {
    ReportOutOfMemory(cx);
    return false;
  }

  SharedModule m = mg.finishModule(BytecodeBufferOrSource(), *moduleMeta,
                                   /*maybeCompleteTier2Listener=*/nullptr);
  if (!m) {
    ReportOutOfMemory(cx);
    return false;
  }

  Rooted<WasmInstanceObject*> instanceObj(cx);
  ImportValues imports;
  if (!m->instantiate(cx, imports, nullptr, &instanceObj)) {
    return false;
  }
  RootedFunction exportedFunc(cx);
  if (!instanceObj->instance().getExportedFunction(cx, /*funcIndex=*/0,
                                                   &exportedFunc)) {
    return false;
  }
  funcObject.setObject(*exportedFunc);

  return true;
}

namespace {
enum ComponentResourceDropEntryStateSlot {
  ComponentResourceDropEntryState_InstanceObj,
  ComponentResourceDropEntryState_CoreFuncIndex,
  ComponentResourceDropEntryState_SlotCount,
};
}  // namespace

static const JSClass ComponentResourceDropEntryStateClass = {
    "ComponentResourceDropEntryState",
    JSCLASS_HAS_RESERVED_SLOTS(ComponentResourceDropEntryState_SlotCount)};

// Implements `canon resource.drop` for an imported resource type: removes
// the handle from the instance's table, trapping on an invalid index, a
// resource type mismatch, or an owning handle with outstanding borrows.
// Imported resource types have no locally-defined destructor to run.
static bool WasmComponentResourceDropEntry(JSContext* cx, unsigned argc,
                                           Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  RootedObject state(cx,
                     &GetFunctionNativeReserved(&args.callee(), 0).toObject());
  Rooted<WasmComponentInstanceObject*> instanceObj(
      cx,
      &JS::GetReservedSlot(state, ComponentResourceDropEntryState_InstanceObj)
           .toObject()
           .as<WasmComponentInstanceObject>());
  // uint32_t coreFuncIndex = uint32_t(
  //     JS::GetReservedSlot(state,
  //     ComponentResourceDropEntryState_CoreFuncIndex)
  //         .toInt32());

  ComponentInstance& instance = instanceObj->instance();
  // const ComponentCoreFuncDesc& desc =
  //     instance.component()->getLoweredCoreFunc(coreFuncIndex);
  // const ComponentResourceBuiltin& builtin =
  //     desc.payload().as<ComponentResourceBuiltin>();
  // MOZ_ASSERT(builtin.kind() == ComponentResourceBuiltin::Kind::ResourceDrop);

  int32_t index;
  if (!JS::ToInt32(cx, args.get(0), &index)) {
    return false;
  }

  if (!instance.handles().drop(cx, uint32_t(index), false)) {
    return false;
  }

  args.rval().setUndefined();
  return true;
}

static JSFunction* BuildResourceDropFunc(
    JSContext* cx, Handle<WasmComponentInstanceObject*> instanceObj,
    uint32_t coreFuncIndex) {
  RootedObject state(cx,
                     JS_NewObject(cx, &ComponentResourceDropEntryStateClass));
  if (!state) {
    return nullptr;
  }
  JS::SetReservedSlot(state, ComponentResourceDropEntryState_InstanceObj,
                      ObjectValue(*instanceObj));
  JS::SetReservedSlot(state, ComponentResourceDropEntryState_CoreFuncIndex,
                      JS::Int32Value(int32_t(coreFuncIndex)));

  RootedFunction fun(
      cx, NewFunctionWithReserved(cx, WasmComponentResourceDropEntry, 1, 0,
                                  nullptr));
  if (!fun) {
    return nullptr;
  }
  SetFunctionNativeReserved(fun, 0, ObjectValue(*state));
  return fun;
}

// Mirrors WasmJS.cpp's own ThrowBadImportArg/ThrowBadImportType and reuses
// the same generic wasm import error messages.
[[nodiscard]] static bool ThrowBadComponentImportArg(JSContext* cx) {
  JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                           JSMSG_WASM_BAD_IMPORT_ARG);
  return false;
}

// `accessPath` is the JS property path the caller must supply (e.g.
// "elementAsNode" or "elementBuffer.prototype.getChild"), not the raw
// component name, since that's what the caller actually needs to fix.
[[nodiscard]] static bool ThrowBadComponentImportType(JSContext* cx,
                                                      const char* accessPath,
                                                      const char* str) {
  JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                           JSMSG_WASM_BAD_IMPORT_TYPE, accessPath, str);
  return false;
}

// Maps a resource type import's raw (kebab) label to the JS value given for
// it at instantiation, so tagged `[constructor]`/`[static]`/`[method]`
// function imports can resolve against it (Web.md "Read the imports
// object"). Small and short-lived (one per ComponentInstance::init call), so
// a linear scan is fine.
class ResourceConstructorTable {
  mozilla::Vector<mozilla::Span<const char>, 0, SystemAllocPolicy> labels_;
  JS::RootedValueVector values_;

 public:
  explicit ResourceConstructorTable(JSContext* cx) : values_(cx) {}

  [[nodiscard]] bool put(mozilla::Span<const char> label, HandleValue value) {
    return labels_.append(label) && values_.append(value);
  }

  bool get(mozilla::Span<const char> label, MutableHandleValue value) const {
    for (size_t i = 0; i < labels_.length(); i++) {
      if (labels_[i] == label) {
        value.set(values_[i]);
        return true;
      }
    }
    return false;
  }
};

// Small and fixed-size: at most a couple of embedders ever register a hook
// in the same process (see JS::AddResourceInterfaceFromValueHook's doc
// comment).
static constexpr size_t MaxResourceInterfaceFromValueHooks = 4;
static JS::ResourceInterfaceFromValueFn
    gResourceInterfaceFromValueHooks[MaxResourceInterfaceFromValueHooks] = {};
static size_t gNumResourceInterfaceFromValueHooks = 0;

void JS::AddResourceInterfaceFromValueHook(ResourceInterfaceFromValueFn fn) {
  // Idempotent: an embedder may run its registration more than once per
  // process (js::DefineTestingFunctions does, once per global), and since
  // ResourceInterfaceFromValue just tries every hook in turn, a duplicate
  // would only burn a slot - and, with a table this small, overflow it.
  for (size_t i = 0; i < gNumResourceInterfaceFromValueHooks; i++) {
    if (gResourceInterfaceFromValueHooks[i] == fn) {
      return;
    }
  }
  MOZ_RELEASE_ASSERT(gNumResourceInterfaceFromValueHooks <
                     MaxResourceInterfaceFromValueHooks);
  gResourceInterfaceFromValueHooks[gNumResourceInterfaceFromValueHooks++] = fn;
}

static JS::WebIDLInterface* ResourceInterfaceFromValue(JSContext* cx,
                                                       HandleValue v) {
  for (size_t i = 0; i < gNumResourceInterfaceFromValueHooks; i++) {
    if (JS::WebIDLInterface* iface =
            gResourceInterfaceFromValueHooks[i](cx, v)) {
      return iface;
    }
  }
  return nullptr;
}

// Resolves each imported resource type against `importObj`: records the JS
// value passed for later `[constructor]`/`[static]`/`[method]` function
// import resolution (Web.md "Read the imports object"), and resolves any
// matching WebIDL interface via the hooks installed by
// AddResourceInterfaceFromValueHook. An import with no value, or a value none
// of the interface hooks recognize, simply leaves the type with no interface -
// its own/borrow coercion falls back to the generic `object` type (see
// CanonicalWebIDLType's Own/Borrow case).
[[nodiscard]] static bool ResolveImportedResourceInterfaces(
    JSContext* cx, const Component& component, HandleObject importObj,
    ResourceConstructorTable* resourceConstructors) {
  if (!importObj) {
    return true;
  }
  for (uint32_t typeIndex = 0; typeIndex < component.types().length();
       typeIndex++) {
    const ComponentItem& item = component.types()[typeIndex];
    if (item.kind() != ComponentItem::ItemKind::Import) {
      continue;
    }
    ComponentType type = component.getType(typeIndex);
    if (type.kind() != ComponentTypeKind::SubResource) {
      continue;
    }
    const ComponentImport& imp = component.imports()[item.itemIndex()];
    JSAtom* nameAtom = KebabToPascalAtom(cx, imp.name().utf8Bytes());
    if (!nameAtom) {
      return false;
    }
    RootedId nameId(cx, AtomToId(nameAtom));
    bool hasProp = false;
    if (!JS_HasPropertyById(cx, importObj, nameId, &hasProp)) {
      return false;
    }
    if (!hasProp) {
      continue;
    }
    RootedValue importVal(cx);
    if (!JS_GetPropertyById(cx, importObj, nameId, &importVal)) {
      return false;
    }
    if (!resourceConstructors->put(imp.name().utf8Bytes(), importVal)) {
      ReportOutOfMemory(cx);
      return false;
    }
    if (JS::WebIDLInterface* iface =
            ResourceInterfaceFromValue(cx, importVal)) {
      bool alreadyBound = false;
      if (!component.tryMarkResourceInterfaceBound(typeIndex, &alreadyBound)) {
        ReportOutOfMemory(cx);
        return false;
      }
      if (alreadyBound) {
        RootedString nameStr(cx, nameAtom);
        UniqueChars nameQuoted = QuoteString(cx, nameStr);
        if (!nameQuoted) {
          ReportOutOfMemory(cx);
          return false;
        }
        JS_ReportErrorASCII(
            cx,
            "resource type import %s already had a WebIDL interface bound "
            "by an earlier instantiation of this component; a Component's "
            "resource types are shared across all its instantiations, so "
            "binding a second (possibly different) interface would "
            "retroactively change every other instantiation's behavior",
            nameQuoted.get());
        return false;
      }
      type.setSubResourceInterface(iface);
    }
  }
  return true;
}

// Reads `id` off `holder`, walking the prototype chain so inherited
// accessors (e.g. a WebIDL getter/setter defined on a parent prototype) are
// found. If the property is an accessor whose getter or setter matches
// `funcType`'s shape (0 params + a result for a getter; 1 param + no result
// for a setter, discounting the `[method]` `self` param when `bindsThis` is
// set), that function is captured directly so it can later be invoked with a
// real receiver instead of being invoked here with `holder` as `this`. Any
// other case (data property, accessor with no shape match, not found) falls
// back to a plain read, unchanged from before.
[[nodiscard]] static bool ReadComponentImportProperty(
    JSContext* cx, HandleObject holder, HandleId id,
    const ComponentFuncType& funcType, bool bindsThis,
    MutableHandleValue importVal) {
  Rooted<mozilla::Maybe<JS::PropertyDescriptor>> desc(cx);
  RootedObject descHolder(cx);
  if (!JS_GetPropertyDescriptorById(cx, holder, id, &desc, &descHolder)) {
    return false;
  }
  if (desc.isSome() && desc->isAccessorDescriptor()) {
    size_t effectiveParams = funcType.paramTypes.length() - (bindsThis ? 1 : 0);
    JSObject* accessor = nullptr;
    if (funcType.resultType.isSome() && effectiveParams == 0 &&
        desc->hasGetter()) {
      accessor = desc->getter();
    } else if (funcType.resultType.isNothing() && effectiveParams == 1 &&
               desc->hasSetter()) {
      accessor = desc->setter();
    }
    if (accessor) {
      importVal.setObject(*accessor);
      return true;
    }
  }
  return JS_GetPropertyById(cx, holder, id, importVal);
}

// Implements the function-import half of Web.md's "Read the imports object":
// plain names are read straight off `importObj` (after kebab->camelCase
// conversion); `[constructor]`/`[static]`/`[method]` names instead resolve
// against the JS value already captured for their resource by
// ResolveImportedResourceInterfaces, falling back to a plain lookup if that
// resource had no JS value imported. `accessPath` is set to the JS property
// path that was (or would have been) read, for diagnostics.
//
// A resolved property that is a getter/setter accessor (rather than a plain
// data property) is captured by ReadComponentImportProperty as the
// getter/setter function itself, picked by matching `funcType`'s shape,
// instead of being invoked with the wrong `this`.
[[nodiscard]] static bool ReadComponentFuncImport(
    JSContext* cx, HandleObject importObj, const CacheableName& name,
    const ComponentFuncType& funcType,
    const ResourceConstructorTable& resourceConstructors,
    MutableHandleValue importVal, UniqueChars* accessPath,
    ComponentNameKind* nameKind) {
  ParsedComponentName parsed = ParseComponentName(name.utf8Bytes());
  *nameKind = parsed.kind;
  bool bindsThis = parsed.kind == ComponentNameKind::Method;

  RootedValue ctor(cx);
  bool haveCtor = parsed.kind != ComponentNameKind::Plain &&
                  resourceConstructors.get(parsed.resource, &ctor);

  UTF8Bytes resourcePascal;
  if (haveCtor && !KebabToPascal(cx, parsed.resource, &resourcePascal)) {
    return false;
  }

  if (haveCtor && parsed.kind == ComponentNameKind::Constructor) {
    importVal.set(ctor);
    *accessPath = JS_smprintf("%.*s constructor", int(resourcePascal.length()),
                              resourcePascal.begin());
    return *accessPath != nullptr;
  }

  if (haveCtor && (parsed.kind == ComponentNameKind::Static ||
                   parsed.kind == ComponentNameKind::Method)) {
    if (!ctor.isObject()) {
      UniqueChars nameQuoted = name.toQuotedString(cx);
      if (!nameQuoted) {
        ReportOutOfMemory(cx);
        return false;
      }
      JS_ReportErrorASCII(
          cx,
          "import %s: resource constructor for \"%.*s\" is not an "
          "object, cannot resolve %s member",
          nameQuoted.get(), int(resourcePascal.length()),
          resourcePascal.begin(),
          parsed.kind == ComponentNameKind::Static ? "static" : "method");
      return false;
    }
    RootedObject holder(cx, &ctor.toObject());
    if (parsed.kind == ComponentNameKind::Method) {
      RootedValue proto(cx);
      if (!JS_GetProperty(cx, holder, "prototype", &proto)) {
        return false;
      }
      if (!proto.isObject()) {
        UniqueChars nameQuoted = name.toQuotedString(cx);
        if (!nameQuoted) {
          ReportOutOfMemory(cx);
          return false;
        }
        JS_ReportErrorASCII(
            cx,
            "import %s: \"%.*s\"'s constructor's prototype is not an "
            "object",
            nameQuoted.get(), int(resourcePascal.length()),
            resourcePascal.begin());
        return false;
      }
      holder = &proto.toObject();
    }
    UTF8Bytes memberCamel;
    if (!KebabToCamel(cx, parsed.member, &memberCamel)) {
      return false;
    }
    JSAtom* memberAtom =
        AtomizeUTF8Chars(cx, memberCamel.begin(), memberCamel.length());
    if (!memberAtom) {
      return false;
    }
    RootedId memberId(cx, AtomToId(memberAtom));
    if (!ReadComponentImportProperty(cx, holder, memberId, funcType, bindsThis,
                                     importVal)) {
      return false;
    }
    *accessPath = JS_smprintf(
        "%.*s.%s%.*s", int(resourcePascal.length()), resourcePascal.begin(),
        parsed.kind == ComponentNameKind::Method ? "prototype." : "",
        int(memberCamel.length()), memberCamel.begin());
    return *accessPath != nullptr;
  }

  UTF8Bytes camel;
  if (!KebabToCamel(cx, name.utf8Bytes(), &camel)) {
    return false;
  }
  JSAtom* atom = AtomizeUTF8Chars(cx, camel.begin(), camel.length());
  if (!atom) {
    return false;
  }
  RootedId id(cx, AtomToId(atom));
  if (!ReadComponentImportProperty(cx, importObj, id, funcType, bindsThis,
                                   importVal)) {
    return false;
  }
  *accessPath = JS_smprintf("%.*s", int(camel.length()), camel.begin());
  return *accessPath != nullptr;
}

bool ComponentInstance::init(JSContext* cx, HandleObject importObj) {
  // We have to instantiate all core instances regardless of whether they are
  // even reachable by the component's exports, because they can have start
  // functions that can trap. (There are probably other reasons too.) This means
  // that we don't really need to do anything clever; we can just iterate over
  // all the core instances defined by the component, and instantiate them in
  // order, with a guarantee from validation that all imports will be satisfied.

  ResourceConstructorTable resourceConstructors(cx);
  if (!ResolveImportedResourceInterfaces(cx, *component_, importObj,
                                         &resourceConstructors)) {
    return false;
  }

  Rooted<WasmComponentInstanceObject*> componentInstanceObj(cx, object());

  // Resolved before any core instance exists: an import resolves to whatever
  // JS callable was passed in under its flat name, and a `canon lift`ed func
  // resolves to a fresh lift closure (BuildLiftedExport) - neither depends on
  // core-instance state, only on the embedder-provided `importObj` and on
  // earlier entries of this same loop (for a re-export). This has to run
  // before the core-instance loop below so that a `canon lower` core func's
  // callee (and thus whether it has a native overload) is already known by
  // the time that loop decides whether to build a native-call stub for it.
  // TODO(wasm-cm): a func reached via an alias into an imported instance
  // isn't representable yet - Component::getTypeForFunc has no Alias case
  // for Func.
  if (!resizeComponentFuncs(component_->funcs().length())) {
    ReportOutOfMemory(cx);
    return false;
  }
  for (uint32_t funcIndex = 0; funcIndex < component_->funcs().length();
       funcIndex++) {
    ComponentItem item = component_->funcs()[funcIndex];
    switch (item.kind()) {
      case ComponentItem::ItemKind::Defined: {
        JSFunction* liftFun =
            BuildLiftedExport(cx, componentInstanceObj, funcIndex);
        if (!liftFun) {
          return false;
        }
        setComponentFunc(funcIndex, liftFun);
      } break;
      case ComponentItem::ItemKind::Import: {
        const ComponentImport& imp = component_->imports()[item.itemIndex()];
        MOZ_RELEASE_ASSERT(imp.externDesc().sort() == ComponentSort::Func);
        ComponentType funcTypeVal = imp.externDesc().asFunc();
        const ComponentFuncType& funcType = funcTypeVal.asFunc();

        if (!importObj) {
          return ThrowBadComponentImportArg(cx);
        }
        UniqueChars accessPath;
        RootedValue importVal(cx);
        ComponentNameKind nameKind;
        if (!ReadComponentFuncImport(cx, importObj, imp.name(), funcType,
                                     resourceConstructors, &importVal,
                                     &accessPath, &nameKind)) {
          return false;
        }
        if (!accessPath) {
          ReportOutOfMemory(cx);
          return false;
        }
        if (!importVal.isObject() || !JS::IsCallable(&importVal.toObject())) {
          return ThrowBadComponentImportType(cx, accessPath.get(), "Function");
        }
        setComponentFunc(funcIndex, &importVal.toObject());
        setComponentFuncNameKind(funcIndex, nameKind);
      } break;
      case ComponentItem::ItemKind::Export: {
        // The re-exported func's index is always less than this Export's own
        // index, so it was already resolved by an earlier iteration of this
        // loop.
        const ComponentExport& exp = component_->exports()[item.itemIndex()];
        MOZ_RELEASE_ASSERT(exp.exported().sort == ComponentSort::Func);
        MOZ_RELEASE_ASSERT(exp.exported().index < funcIndex);
        setComponentFunc(funcIndex, componentFunc(exp.exported().index));
      } break;
      default:
        MOZ_CRASH("component-level func aliases are not supported yet");
    }
  }

  if (!resizeLoweredNativeClosures(component_->coreFuncs().length())) {
    ReportOutOfMemory(cx);
    return false;
  }

  if (!coreInstances_.resize(component_->coreInstances().length())) {
    ReportOutOfMemory(cx);
    return false;
  }

  for (uint32_t i = 0; i < component_->coreInstances().length(); i++) {
    const CoreInstanceDesc& instanceDesc = component_->getCoreInstance(i);
    if (!instanceDesc.desc().is<CoreInstanceDescFromModule>()) {
      continue;
    }
    const CoreInstanceDescFromModule& desc =
        instanceDesc.desc().as<CoreInstanceDescFromModule>();
    SharedModule mod = component_->getCoreModule(desc.moduleIndex);

    // Parallel to the module's own func-import index space (see
    // CodeMetadata::componentImportHints): captures the runtime pointer a
    // ComponentImportKind-classified import's inlined call site will load
    // from ComponentImportInstanceData - a LoweredNativeClosure* for
    // CanonLower, this ComponentInstance for ResourceDrop. Left null for
    // every import None applies to. Written into the new core instance's
    // instance data once it exists, below.
    mozilla::Vector<void*, 0, SystemAllocPolicy> componentImportPtrs;
    if (!mod->codeMeta().componentImportHints.empty() &&
        !componentImportPtrs.resize(mod->codeMeta().numFuncImports)) {
      ReportOutOfMemory(cx);
      return false;
    }
    uint32_t funcImportIndex = 0;

    ImportValues imports;
    for (const Import& imp : mod->moduleMeta().imports) {
      uint32_t thisFuncImportIndex = funcImportIndex;
      if (imp.kind == DefinitionKind::Function) {
        funcImportIndex++;
      }

      auto p = desc.args.lookup(imp.module.utf8Bytes());
      MOZ_RELEASE_ASSERT(p);
      uint32_t instanceIndex = p->value();
      Rooted<WasmInstanceObject*> sourceInstance(cx,
                                                 coreInstances_[instanceIndex]);

      RootedValue exportVal(cx);
      if (sourceInstance) {
        RootedObject exportsObj(cx, &sourceInstance->exportsObj());
        if (!GetValidatedNamedPropertyFromObject(cx, exportsObj, imp.field,
                                                 &exportVal)) {
          return false;
        }
      } else {
        const ComponentInlineExports& inlineExports =
            component_->getCoreInstance(instanceIndex)
                .desc()
                .as<ComponentInlineExports>();
        ComponentSortIndex actualItemSortIndex =
            inlineExports.mustResolveExportToOriginal(imp.field);
        ComponentItem actualItem =
            component_->resolveSortIndex(actualItemSortIndex);

        // Resolve outer aliases first: by construction there are never chains
        // of inline exports, only at most one level of outer alias.
        if (actualItem.isOuterAlias()) {
          MOZ_RELEASE_ASSERT(actualItem.aliasInstanceIndex() == 0);
          actualItemSortIndex = actualItem.outerAliasSortIndex();
          actualItem = component_->resolveSortIndex(actualItemSortIndex);
        }

        switch (actualItem.kind()) {
          case ComponentItem::ItemKind::Defined: {
            MOZ_RELEASE_ASSERT(actualItem.sort() ==
                               ComponentSort::CoreFunction);

            const ComponentCoreFuncDesc& coreFuncDesc =
                component_->getLoweredCoreFunc(actualItemSortIndex.index);
            if (coreFuncDesc.payload().is<uint32_t>()) {
              if (!ResolveLoweredCoreFunc(cx, componentInstanceObj,
                                          actualItemSortIndex.index,
                                          &exportVal)) {
                return false;
              }
              if (!componentImportPtrs.empty()) {
                MOZ_ASSERT(mod->codeMeta()
                               .componentImportHint(thisFuncImportIndex)
                               .kind == ComponentImportKind::CanonLower);
                // Always non-null: ResolveLoweredCoreFunc installs a
                // LoweredNativeClosure for every `canon lower` core func,
                // whether or not its actual callee turned out native-
                // eligible (see SelectNativeOverload) - an ineligible one's
                // closure calls the generic entry instead. The inlined call
                // site (see emitCallComponentCanonLower) can therefore call
                // straight through it unconditionally.
                componentImportPtrs[thisFuncImportIndex] =
                    loweredNativeClosure(actualItemSortIndex.index);
                MOZ_ASSERT(componentImportPtrs[thisFuncImportIndex]);
              }
            } else if (const ComponentResourceBuiltin& builtin =
                           coreFuncDesc.payload()
                               .as<ComponentResourceBuiltin>();
                       builtin.kind() ==
                           ComponentResourceBuiltin::Kind::ResourceDrop &&
                       builtin.resourceType().kind() ==
                           ComponentTypeKind::SubResource) {
              // Only imported resource types are wired up for real: a
              // locally-defined resource's dtor (if any) isn't called yet, so
              // those keep falling through to the trapping stub below.
              const TypeDef& coreFuncTypeDef =
                  component_->getTypeForCoreFunc(actualItemSortIndex.index);
              bool baselineUnavailable = false;
              if (!GenerateResourceDropFunc(cx, &coreFuncTypeDef,
                                            &componentInstanceObj->instance(),
                                            &baselineUnavailable, &exportVal)) {
                return false;
              }
              if (baselineUnavailable) {
                JSFunction* dropFun = BuildResourceDropFunc(
                    cx, componentInstanceObj, actualItemSortIndex.index);
                if (!dropFun) {
                  return false;
                }
                exportVal.setObject(*dropFun);
              }
              if (!componentImportPtrs.empty()) {
                MOZ_ASSERT(mod->codeMeta()
                               .componentImportHint(thisFuncImportIndex)
                               .kind == ComponentImportKind::ResourceDrop);
                // Always available, regardless of `baselineUnavailable`: the
                // inlined call site calls Instance::componentResourceDrop
                // directly (see emitCallComponentResourceDrop), it never
                // goes through the GenerateResourceDropFunc stub this
                // pointer would otherwise only be needed for.
                componentImportPtrs[thisFuncImportIndex] =
                    &componentInstanceObj->instance();
              }
            } else {
              // TODO(wasm-cm): resource.new/rep, and resource.drop for a
              // locally-defined resource, aren't wired up yet; keep
              // synthesizing a function with the correct signature that just
              // traps.
              const TypeDef& coreFuncTypeDef =
                  component_->getTypeForCoreFunc(actualItemSortIndex.index);
              if (!GenerateDummyLoweredFunc(cx, &coreFuncTypeDef, &exportVal)) {
                return false;
              }
            }
          } break;
          case ComponentItem::ItemKind::Alias: {
            MOZ_RELEASE_ASSERT(actualItem.aliasKind() ==
                               ComponentAliasKind::CoreExport);
            MOZ_RELEASE_ASSERT(coreInstances_[actualItem.aliasInstanceIndex()]);

            RootedObject exportsObj(
                cx,
                &coreInstances_[actualItem.aliasInstanceIndex()]->exportsObj());
            const CacheableName& exportName =
                component_->getExportNameForAlias(actualItemSortIndex);
            if (!GetValidatedNamedPropertyFromObject(cx, exportsObj, exportName,
                                                     &exportVal)) {
              return false;
            }
          } break;
          default:
            MOZ_CRASH();
        }

        // TODO(wasm-cm): All of this seems like it shouldn't really be
        // necessary to look up per import and per instantiation. If we end up
        // "pre-baking" instantiation instructions in the component, this would
        // be a strong candidate to move there.
      }

      switch (imp.kind) {
        case DefinitionKind::Function: {
          if (!imports.funcs.append(&exportVal.toObject().as<JSFunction>())) {
            ReportOutOfMemory(cx);
            return false;
          }
        } break;
        case DefinitionKind::Table: {
          if (!imports.tables.append(
                  &exportVal.toObject().as<WasmTableObject>())) {
            ReportOutOfMemory(cx);
            return false;
          }
        } break;
        case DefinitionKind::Memory: {
          if (!imports.memories.append(
                  &exportVal.toObject().as<WasmMemoryObject>())) {
            ReportOutOfMemory(cx);
            return false;
          }
        } break;
        case DefinitionKind::Global: {
          Rooted<WasmGlobalObject*> global(
              cx, &exportVal.toObject().as<WasmGlobalObject>());
          if (!imports.globalObjs.append(global) ||
              !imports.globalValues.append(global->val())) {
            ReportOutOfMemory(cx);
            return false;
          }
        } break;
        case DefinitionKind::Tag: {
          if (!imports.tagObjs.append(
                  &exportVal.toObject().as<WasmTagObject>())) {
            ReportOutOfMemory(cx);
            return false;
          }
        } break;
        default:
          MOZ_CRASH();
      }
    }

    RootedObject instanceProto(
        cx, GlobalObject::getOrCreatePrototype(cx, JSProto_WasmInstance));
    if (!instanceProto) {
      return false;
    }
    Rooted<WasmInstanceObject*> instanceObj(cx);
    if (!mod->instantiate(cx, imports, instanceProto, &instanceObj)) {
      return false;
    }
    coreInstances_[i] = instanceObj;

    for (uint32_t j = 0; j < componentImportPtrs.length(); j++) {
      if (!componentImportPtrs[j]) {
        continue;
      }
      instanceObj->instance().componentImportInstanceData(j).ptr =
          componentImportPtrs[j];
    }
  }

  // Only now that every core instance exists can a closure's memory/realloc
  // be resolved to their live core exports - do that in one last pass over
  // every `canon lower` core func's closure (see ResolveLoweredCoreFunc,
  // called from the core-instance loop above); still skips a null entry,
  // which means this core func is a resource builtin instead.
  for (uint32_t coreFuncIndex = 0;
       coreFuncIndex < loweredNativeClosures_.length(); coreFuncIndex++) {
    LoweredNativeClosure* closure = loweredNativeClosure(coreFuncIndex);
    if (!closure) {
      continue;
    }
    const ComponentCoreFuncDesc& desc =
        component_->getLoweredCoreFunc(coreFuncIndex);
    const ComponentCanonOpts& opts = desc.canonOpts();

    Rooted<WasmMemoryObject*> memory(cx);
    if (opts.memoryIndex) {
      if (!resolveCoreMemory(*opts.memoryIndex, &memory)) {
        return false;
      }
    }
    RootedObject realloc(cx);
    if (opts.reallocIndex) {
      if (!resolveCoreFunc(*opts.reallocIndex, &realloc)) {
        return false;
      }
    }

    uint8_t* memoryBase = nullptr;
    if (memory) {
      MOZ_RELEASE_ASSERT(!memory->isShared() && !memory->movingGrowable());
      memoryBase = memory->buffer().dataPointerEither().unwrap();
    }
    closure->ctx.setMemoryAndRealloc(memory, memoryBase, realloc);
  }

  RootedObject exportsObj(cx, NewPlainObjectWithProto(cx, nullptr));
  if (!exportsObj) {
    return false;
  }
  uint8_t propertyAttr = JSPROP_ENUMERATE | JSPROP_READONLY | JSPROP_PERMANENT;
  for (const ComponentExport& exp : component_->exports()) {
    if (exp.externDesc().sort() != ComponentSort::Func) {
      // TODO(wasm-cm): only func exports are surfaced for now.
      continue;
    }
    MOZ_RELEASE_ASSERT(exp.exported().sort == ComponentSort::Func);
    // Bracket-tagged (resource method/constructor) names aren't surfaced as
    // WebIDL interface members yet (only plain func exports are), so leave
    // them unconverted for now.
    ParsedComponentName parsed = ParseComponentName(exp.name().utf8Bytes());
    JSAtom* atom = parsed.kind == ComponentNameKind::Plain
                       ? KebabToCamelAtom(cx, exp.name().utf8Bytes())
                       : exp.name().toAtom(cx);
    if (!atom) {
      return false;
    }
    RootedId id(cx, AtomToId(atom));
    RootedValue val(cx, ObjectValue(*componentFunc(exp.exported().index)));
    if (!JS_DefinePropertyById(cx, exportsObj, id, val, propertyAttr)) {
      return false;
    }
  }
  if (!PreventExtensions(cx, exportsObj)) {
    return false;
  }
  componentInstanceObj->initExportsObj(*exportsObj);

  return true;
}

static const char* ComponentSortKeyword(ComponentSort sort) {
  switch (sort) {
    case ComponentSort::Func:
      return "func";
    case ComponentSort::Type:
      return "type";
    case ComponentSort::Component:
      return "component";
    case ComponentSort::Instance:
      return "instance";
    case ComponentSort::CoreFunction:
      return "core func";
    case ComponentSort::CoreTable:
      return "core table";
    case ComponentSort::CoreMemory:
      return "core memory";
    case ComponentSort::CoreGlobal:
      return "core global";
    case ComponentSort::CoreTag:
      return "core tag";
    case ComponentSort::CoreType:
      return "core type";
    case ComponentSort::CoreModule:
      return "core module";
    case ComponentSort::CoreInstance:
      return "core instance";
    case ComponentSort::Invalid:
      return "invalid";

    default:
      MOZ_CRASH();
  }
}

static const char* ComponentAliasKindKeyword(ComponentAliasKind kind) {
  switch (kind) {
    case ComponentAliasKind::CoreExport:
      return "core export";
    case ComponentAliasKind::Export:
      return "export";
    case ComponentAliasKind::Outer:
      return "outer";

    default:
      MOZ_CRASH();
  }
}

UniqueChars wasm::ToString(ComponentItem item) {
  const char* sort = ComponentSortKeyword(item.sort());
  switch (item.kind()) {
    case ComponentItem::ItemKind::Invalid:
      return JS_smprintf("(invalid)");
    case ComponentItem::ItemKind::Defined:
      return JS_smprintf("(%s (defined %u))", sort, item.itemIndex());
    case ComponentItem::ItemKind::Import:
      return JS_smprintf("(%s (import %u))", sort, item.itemIndex());
    case ComponentItem::ItemKind::Export:
      return JS_smprintf("(%s (export %u))", sort, item.itemIndex());
    case ComponentItem::ItemKind::Alias:
      return JS_smprintf("(alias %s %u (%s %u))",
                         ComponentAliasKindKeyword(item.aliasKind()),
                         item.aliasInstanceIndex(), sort, item.itemIndex());

    default:
      MOZ_CRASH();
  }
}

UniqueChars wasm::ToString(ComponentSortIndex sortIndex) {
  return JS_smprintf("(%s %u)", ComponentSortKeyword(sortIndex.sort),
                     sortIndex.index);
}

ComponentHandleTable::~ComponentHandleTable() {
  for (uint32_t i = 0; i < length_; i++) {
    Entry& entry = entries_[i];
    if ((entry.flags & (Present | Native)) == (Present | Native)) {
      entry.iface->releaseNative(entry.ptr);
    }
  }
  js_free(entries_);
  js_free(freeStack_);
}

static ComponentTypeKind KindFromFlags(uint32_t flags) {
  return ComponentTypeKind((flags & ComponentHandleTable::KindMask) >>
                           ComponentHandleTable::KindShift);
}
static uint32_t FlagsFromKind(ComponentTypeKind kind) {
  return (uint32_t(kind) << ComponentHandleTable::KindShift) &
         ComponentHandleTable::KindMask;
}
static ComponentResourceTypeId EntryResourceTypeId(
    const ComponentHandleTable::Entry& entry) {
  return ComponentResourceTypeId(KindFromFlags(entry.flags),
                                 entry.resourceTypeDef);
}

bool ComponentHandleTable::growEntries(JSContext* cx) {
  uint32_t newCapacity = capacity_ == 0 ? 8 : capacity_ * 2;
  if (newCapacity > MaxLength) {
    newCapacity = MaxLength;
  }
  if (newCapacity <= capacity_) {
    JS_ReportErrorASCII(cx, "component resource handle table is full");
    return false;
  }
  Entry* newEntries = js_pod_realloc<Entry>(entries_, capacity_, newCapacity);
  if (!newEntries) {
    ReportOutOfMemory(cx);
    return false;
  }
  entries_ = newEntries;
  capacity_ = newCapacity;
  return true;
}

bool ComponentHandleTable::pushFree(JSContext* cx, uint32_t index) {
  if (freeLength_ == freeCapacity_) {
    uint32_t newCapacity = freeCapacity_ == 0 ? 8 : freeCapacity_ * 2;
    uint32_t* newStack =
        js_pod_realloc<uint32_t>(freeStack_, freeCapacity_, newCapacity);
    if (!newStack) {
      ReportOutOfMemory(cx);
      return false;
    }
    freeStack_ = newStack;
    freeCapacity_ = newCapacity;
  }
  freeStack_[freeLength_] = index;
  freeLength_++;
  return true;
}

bool ComponentHandleTable::reserve(JSContext* cx, uint32_t* index) {
  if (length_ == 0) {
    // Index 0 is a reserved sentinel, never allocated.
    if (!growEntries(cx)) {
      return false;
    }
    if (!wrapped_.resize(1)) {
      ReportOutOfMemory(cx);
      return false;
    }
    entries_[0] = Entry();
    length_ = 1;
  }
  if (freeLength_ > 0) {
    freeLength_--;
    uint32_t i = freeStack_[freeLength_];
    MOZ_ASSERT(!(entries_[i].flags & Present));
    *index = i;
    return true;
  }
  if (length_ >= MaxLength) {
    JS_ReportErrorASCII(cx, "component resource handle table is full");
    return false;
  }
  if (length_ == capacity_ && !growEntries(cx)) {
    return false;
  }
  if (!wrapped_.resize(length_ + 1)) {
    ReportOutOfMemory(cx);
    return false;
  }
  entries_[length_] = Entry();
  *index = length_;
  length_++;
  return true;
}

bool ComponentHandleTable::add(JSContext* cx,
                               ComponentResourceTypeId resourceType, bool own,
                               const ComponentResourceClassification& value,
                               uint32_t* index) {
  uint32_t i;
  if (!reserve(cx, &i)) {
    return false;
  }
  Entry& entry = entries_[i];
  entry.flags = Present | (own ? Own : 0) | FlagsFromKind(resourceType.kind());
  entry.numLends = 0;
  entry.resourceTypeDef = resourceType.typeDef();
  if (value.isNative) {
    entry.flags |= Native;
    entry.ptr = value.nativePtr;
    entry.iface = value.nativeIface;
    MOZ_ASSERT(!wrapped_[i]);
    // wrapped_[i] = nullptr;
    if (!value.adoptRef) {
      value.nativeIface->addRefNative(value.nativePtr);
    }
  } else {
    MOZ_ASSERT(!entry.ptr);
    MOZ_ASSERT(!entry.iface);
    wrapped_[i] = value.wrappedObj;
  }
  *index = i;
  return true;
}

static void FillHandleView(const ComponentHandleTable::Entry& entry,
                           JSObject* wrappedObj, ComponentResourceHandle* out) {
  out->resourceType = EntryResourceTypeId(entry);
  out->own = entry.flags & ComponentHandleTable::Own;
  out->numLends = entry.numLends;
  out->isNative = entry.flags & ComponentHandleTable::Native;
  if (out->isNative) {
    out->nativePtr = entry.ptr;
    out->nativeIface = entry.iface;
  } else {
    out->wrappedObj = wrappedObj;
  }
}

bool ComponentHandleTable::get(JSContext* cx, uint32_t index,
                               ComponentResourceHandle* out) const {
  if (index >= length_ || !(entries_[index].flags & Present)) {
    JS_ReportErrorASCII(cx, "invalid component resource handle");
    return false;
  }
  FillHandleView(entries_[index], wrapped_[index], out);
  return true;
}

bool ComponentHandleTable::remove(JSContext* cx, uint32_t index,
                                  ComponentResourceHandle* out) {
  if (index >= length_ || !(entries_[index].flags & Present)) {
    JS_ReportErrorASCII(cx, "invalid component resource handle");
    return false;
  }
  FillHandleView(entries_[index], wrapped_[index], out);
  entries_[index].flags = 0;
  if ((entries_[index].flags & ComponentHandleTable::Native) != 0) {
    entries_[index].ptr = nullptr;
    entries_[index].iface = nullptr;
    MOZ_ASSERT(!wrapped_[index]);
  } else {
    MOZ_ASSERT(!entries_[index].ptr);
    MOZ_ASSERT(!entries_[index].iface);
    wrapped_[index] = nullptr;
  }
  return pushFree(cx, index);
}

bool ComponentHandleTable::drop(JSContext* cx, uint32_t index,
                                bool requireOwn) {
  if (index >= length_ || !(entries_[index].flags & Present)) {
    JS_ReportErrorASCII(cx, "invalid component resource handle");
    return false;
  }
  Entry& entry = entries_[index];
  // TODO: add this back
  // if (KindFromFlags(entry.flags) != kind || entry.resourceTypeDef != def) {
  //   JS_ReportErrorASCII(cx, "component resource handle type mismatch");
  //   return false;
  // }
  bool own = entry.flags & Own;
  if (requireOwn && !own) {
    JS_ReportErrorASCII(cx, "component resource handle is a borrow");
    return false;
  }
  if (entry.numLends != 0) {
    JS_ReportErrorASCII(cx,
                        "component resource handle still has active borrows");
    return false;
  }
  if (entry.flags & Native) {
    entry.iface->releaseNative(entry.ptr);
    entry.ptr = nullptr;
    entry.iface = nullptr;
    MOZ_ASSERT(!wrapped_[index]);
  } else {
    MOZ_ASSERT(!entry.ptr);
    MOZ_ASSERT(!entry.iface);
    wrapped_[index] = nullptr;
  }
  entry.flags = 0;

  return pushFree(cx, index);
}

ComponentValue ComponentValue::makeBool(const ComponentType& type, bool b) {
  return ComponentValue(type, Repr(b));
}
ComponentValue ComponentValue::makeSigned(const ComponentType& type,
                                          int64_t v) {
  return ComponentValue(type, Repr(v));
}
ComponentValue ComponentValue::makeUnsigned(const ComponentType& type,
                                            uint64_t v) {
  return ComponentValue(type, Repr(v));
}
ComponentValue ComponentValue::makeF32(const ComponentType& type, float v) {
  return ComponentValue(type, Repr(v));
}
ComponentValue ComponentValue::makeF64(const ComponentType& type, double v) {
  return ComponentValue(type, Repr(v));
}
ComponentValue ComponentValue::makeComposite(const ComponentType& type,
                                             RefPtr<ComponentValueData> data) {
  return ComponentValue(type, Repr(std::move(data)));
}

using JS::WebIDLType;
using JS::WebIDLTypeKind;
using JS::WebIDLValue;
using JS::WebIDLValueData;

static bool ReportBridgeUnsupported(JSContext* cx, const char* what) {
  JS_ReportErrorASCII(cx, "component<->WebIDL bridge: %s not yet supported",
                      what);
  return false;
}

static WebIDLTypeKind CanonicalIntegerKind(ComponentTypeKind kind) {
  switch (kind) {
    case ComponentTypeKind::S8:
      return WebIDLTypeKind::Byte;
    case ComponentTypeKind::U8:
      return WebIDLTypeKind::Octet;
    case ComponentTypeKind::S16:
      return WebIDLTypeKind::Short;
    case ComponentTypeKind::U16:
      return WebIDLTypeKind::UnsignedShort;
    case ComponentTypeKind::S32:
      return WebIDLTypeKind::Long;
    case ComponentTypeKind::U32:
      return WebIDLTypeKind::UnsignedLong;
    case ComponentTypeKind::S64:
      return WebIDLTypeKind::LongLong;
    case ComponentTypeKind::U64:
      return WebIDLTypeKind::UnsignedLongLong;
    default:
      MOZ_CRASH("not a component integer kind");
  }
}

static JS::UniqueChars CacheableNameToUniqueChars(const CacheableName& name) {
  mozilla::Span<const char> span = name.utf8Bytes();
  JS::UniqueChars buf(js_pod_malloc<char>(span.Length() + 1));
  if (!buf) {
    return nullptr;
  }
  for (size_t i = 0; i < span.Length(); i++) {
    buf[i] = span[i];
  }
  buf[span.Length()] = '\0';
  return buf;
}

struct BridgeVariantCase {
  JS::UniqueChars label;
  mozilla::Maybe<ComponentType> type;
};
using BridgeVariantCaseVector =
    mozilla::Vector<BridgeVariantCase, 2, SystemAllocPolicy>;

// Builds the WebIDL type Web.md's CanonicalWebIDLType assigns to a
// generic-variant-shaped component type: an anonymous `enumeration KindEnum`
// plus a `dictionary {kind, value?}`. Shared by variant, result, and nested
// option<option<T>> (which must despecialize to this form since WebIDL null
// can't distinguish none from some(none)).
static bool CanonicalVariantType(JSContext* cx, BridgeVariantCaseVector&& cases,
                                 WebIDLType* out) {
  mozilla::Vector<JS::UniqueChars, 0, SystemAllocPolicy> labels;
  JS::WebIDLTypeVector payloadTypes;
  for (BridgeVariantCase& c : cases) {
    if (!labels.append(std::move(c.label))) {
      return false;
    }
    if (c.type) {
      WebIDLType payloadType;
      if (!CanonicalWebIDLType(cx, *c.type, &payloadType) ||
          !payloadTypes.append(payloadType)) {
        return false;
      }
    }
  }

  WebIDLType kindEnum;
  if (!WebIDLType::enumeration(std::move(labels), &kindEnum)) {
    return false;
  }
  if (!WebIDLType::dictionary(out)) {
    return false;
  }
  JS::UniqueChars kindName = DuplicateString(cx, "kind");
  if (!kindName ||
      !out->addDictionaryMember(std::move(kindName), kindEnum, true)) {
    return false;
  }
  if (payloadTypes.empty()) {
    return true;
  }
  WebIDLType valueType = payloadTypes[0];
  if (payloadTypes.length() > 1 &&
      !WebIDLType::union_(std::move(payloadTypes), &valueType)) {
    return false;
  }
  JS::UniqueChars valueName = DuplicateString(cx, "value");
  return valueName != nullptr &&
         out->addDictionaryMember(std::move(valueName), valueType, false);
}

bool js::wasm::CanonicalWebIDLType(JSContext* cx,
                                   const ComponentType& componentType,
                                   WebIDLType* out) {
  ComponentTypeKind kind = componentType.kind();
  switch (kind) {
    case ComponentTypeKind::Bool:
      *out = WebIDLType::primitive(WebIDLTypeKind::Boolean);
      return true;
    case ComponentTypeKind::S8:
    case ComponentTypeKind::U8:
    case ComponentTypeKind::S16:
    case ComponentTypeKind::U16:
    case ComponentTypeKind::S32:
    case ComponentTypeKind::U32:
    case ComponentTypeKind::S64:
    case ComponentTypeKind::U64:
      *out = WebIDLType::primitive(CanonicalIntegerKind(kind));
      return true;
    case ComponentTypeKind::F32:
      *out = WebIDLType::primitive(WebIDLTypeKind::UnrestrictedFloat);
      return true;
    case ComponentTypeKind::F64:
      *out = WebIDLType::primitive(WebIDLTypeKind::UnrestrictedDouble);
      return true;
    case ComponentTypeKind::Char:
    case ComponentTypeKind::String:
      *out = WebIDLType::primitive(WebIDLTypeKind::USVString);
      return true;
    case ComponentTypeKind::List: {
      WebIDLType element;
      if (!CanonicalWebIDLType(cx, componentType.asList(), &element)) {
        return false;
      }
      return WebIDLType::sequence(element, out);
    }
    case ComponentTypeKind::Record: {
      if (!WebIDLType::dictionary(out)) {
        return false;
      }
      for (const ComponentRecordField& field : componentType.asRecord()) {
        WebIDLType fieldType;
        if (!CanonicalWebIDLType(cx, field.type, &fieldType)) {
          return false;
        }
        JS::UniqueChars name = CacheableNameToUniqueChars(field.name);
        if (!name ||
            !out->addDictionaryMember(std::move(name), fieldType, true)) {
          return false;
        }
      }
      return true;
    }
    case ComponentTypeKind::Tuple: {
      JS::WebIDLTypeVector members;
      for (const ComponentType& item : componentType.asTuple()) {
        WebIDLType itemType;
        if (!CanonicalWebIDLType(cx, item, &itemType) ||
            !members.append(itemType)) {
          return false;
        }
      }
      WebIDLType elementUnion;
      if (!WebIDLType::union_(std::move(members), &elementUnion)) {
        return false;
      }
      return WebIDLType::sequence(elementUnion, out);
    }
    case ComponentTypeKind::Flags: {
      if (!WebIDLType::dictionary(out)) {
        return false;
      }
      for (const CacheableName& label : componentType.asFlags()) {
        JS::UniqueChars name = CacheableNameToUniqueChars(label);
        if (!name ||
            !out->addDictionaryMember(
                std::move(name), WebIDLType::primitive(WebIDLTypeKind::Boolean),
                false)) {
          return false;
        }
      }
      return true;
    }
    case ComponentTypeKind::Enum: {
      mozilla::Vector<JS::UniqueChars, 0, SystemAllocPolicy> labels;
      for (const CacheableName& label : componentType.asEnum()) {
        JS::UniqueChars name = CacheableNameToUniqueChars(label);
        if (!name || !labels.append(std::move(name))) {
          return false;
        }
      }
      return WebIDLType::enumeration(std::move(labels), out);
    }
    case ComponentTypeKind::Option: {
      ComponentType inner = componentType.asOption();
      if (inner.kind() == ComponentTypeKind::Option) {
        // A nested option must despecialize to the variant form: a WebIDL
        // `null` can't distinguish `none` from `some(none)`.
        JS::UniqueChars noneLabel = DuplicateString(cx, "none");
        JS::UniqueChars someLabel = DuplicateString(cx, "some");
        BridgeVariantCaseVector cases;
        if (!noneLabel || !someLabel ||
            !cases.append(
                BridgeVariantCase{std::move(noneLabel), mozilla::Nothing()}) ||
            !cases.append(BridgeVariantCase{std::move(someLabel),
                                            mozilla::Some(inner)})) {
          return false;
        }
        return CanonicalVariantType(cx, std::move(cases), out);
      }
      WebIDLType innerCanonical;
      if (!CanonicalWebIDLType(cx, inner, &innerCanonical)) {
        return false;
      }
      return WebIDLType::nullable(innerCanonical, out);
    }
    case ComponentTypeKind::Variant: {
      BridgeVariantCaseVector cases;
      for (const ComponentVariantCase& c : componentType.asVariant()) {
        JS::UniqueChars label = CacheableNameToUniqueChars(c.name);
        if (!label ||
            !cases.append(BridgeVariantCase{std::move(label), c.type})) {
          return false;
        }
      }
      return CanonicalVariantType(cx, std::move(cases), out);
    }
    case ComponentTypeKind::Result: {
      ComponentResultType result = componentType.asResult();
      JS::UniqueChars okLabel = DuplicateString(cx, "ok");
      JS::UniqueChars errorLabel = DuplicateString(cx, "error");
      BridgeVariantCaseVector cases;
      if (!okLabel || !errorLabel ||
          !cases.append(BridgeVariantCase{std::move(okLabel), result.type}) ||
          !cases.append(
              BridgeVariantCase{std::move(errorLabel), result.errorType})) {
        return false;
      }
      return CanonicalVariantType(cx, std::move(cases), out);
    }
    case ComponentTypeKind::Own:
    case ComponentTypeKind::Borrow: {
      // Per Web.md's "Resource types": a resource without a WebIDL
      // constructor uses the generic `object` type. This applies to both a
      // locally-defined resource (Resource) and an imported one
      // (SubResource, whose interface - if any - is resolved from the
      // import object during instantiation; see setSubResourceInterface).
      ComponentType resource = kind == ComponentTypeKind::Own
                                   ? componentType.asOwn()
                                   : componentType.asBorrow();
      JS::WebIDLInterface* iface =
          (resource.kind() == ComponentTypeKind::Resource ||
           resource.kind() == ComponentTypeKind::SubResource)
              ? resource.asResource().interface()
              : nullptr;
      if (!iface) {
        *out = WebIDLType::primitive(WebIDLTypeKind::Object);
        return true;
      }
      return WebIDLType::interface_(iface, out);
    }
    default:
      return ReportBridgeUnsupported(cx, "this component type");
  }
}

bool js::wasm::CanonicalWebIDLOperationType(JSContext* cx,
                                            const ComponentFuncType& funcType,
                                            JS::WebIDLOperationType* out) {
  JS::WebIDLParamVector params;
  if (!params.reserve(funcType.paramTypes.length())) {
    ReportOutOfMemory(cx);
    return false;
  }
  for (const ComponentType& paramType : funcType.paramTypes) {
    WebIDLType webidlType;
    if (!CanonicalWebIDLType(cx, paramType, &webidlType)) {
      return false;
    }
    // A component function's arity is fixed, so every canonical param is
    // required and non-variadic.
    params.infallibleAppend(JS::WebIDLParam{webidlType});
  }

  mozilla::Maybe<WebIDLType> returnType;
  if (funcType.resultType.isSome()) {
    WebIDLType webidlType;
    if (!CanonicalWebIDLType(cx, funcType.resultType.ref(), &webidlType)) {
      return false;
    }
    returnType = mozilla::Some(webidlType);
  }

  out->params = std::move(params);
  out->returnType = returnType;
  return true;
}

static JSString* CodePointToJSString(JSContext* cx, uint32_t cp) {
  char16_t buf[2];
  size_t n;
  if (cp <= 0xFFFF) {
    buf[0] = char16_t(cp);
    n = 1;
  } else {
    cp -= 0x10000;
    buf[0] = char16_t(0xD800 + (cp >> 10));
    buf[1] = char16_t(0xDC00 + (cp & 0x3FF));
    n = 2;
  }
  return JS_NewUCStringCopyN(cx, buf, n);
}

// Builds the WebIDL value for a generic-variant-shaped case (variant/result/
// nested option), given the canonical dictionary type and the resolved case
// index/payload.
static bool ToCanonicalVariantLikeValue(
    JSContext* cx, const WebIDLType& canonical, uint32_t caseIndex,
    const mozilla::Maybe<ComponentValue>& payload,
    JS::MutableHandleWebIDLValue out, ComponentHandleTable* handles) {
  RefPtr<WebIDLValueData> data =
      js_new<WebIDLValueData>(WebIDLValueData::DictionaryData{});
  if (!data) {
    ReportOutOfMemory(cx);
    return false;
  }
  out.set(WebIDLValue::makeComposite(canonical, data));

  const JS::WebIDLDictionaryType& dict =
      canonical.def()->schema().as<JS::WebIDLDictionaryType>();
  if (!data->appendDictionaryMember(
          WebIDLValue::makeEnum(dict.members[0].type, caseIndex))) {
    return false;
  }
  if (dict.members.length() < 2) {
    return true;
  }
  if (payload) {
    JS::RootedWebIDLValue valueOut(cx);
    return ToCanonicalWebIDLValue(cx, *payload, &valueOut, handles) &&
           data->appendDictionaryMember(valueOut.get());
  }
  return data->appendDictionaryMember(WebIDLValue());
}

bool js::wasm::ToCanonicalWebIDLValue(JSContext* cx,
                                      const ComponentValue& value,
                                      JS::MutableHandleWebIDLValue out,
                                      ComponentHandleTable* handles) {
  const ComponentType& ct = value.type();
  WebIDLType canonical;
  if (!CanonicalWebIDLType(cx, ct, &canonical)) {
    return false;
  }

  switch (ct.kind()) {
    case ComponentTypeKind::Bool:
      out.set(WebIDLValue::makeBool(canonical, value.asBool()));
      return true;
    case ComponentTypeKind::S8:
    case ComponentTypeKind::S16:
    case ComponentTypeKind::S32:
    case ComponentTypeKind::S64:
      out.set(WebIDLValue::makeSigned(canonical, value.asSigned()));
      return true;
    case ComponentTypeKind::U8:
    case ComponentTypeKind::U16:
    case ComponentTypeKind::U32:
    case ComponentTypeKind::U64:
      out.set(WebIDLValue::makeUnsigned(canonical, value.asUnsigned()));
      return true;
    case ComponentTypeKind::F32:
      out.set(WebIDLValue::makeDouble(canonical, double(value.asF32())));
      return true;
    case ComponentTypeKind::F64:
      out.set(WebIDLValue::makeDouble(canonical, value.asF64()));
      return true;
    case ComponentTypeKind::Char: {
      JSString* str = CodePointToJSString(cx, uint32_t(value.asUnsigned()));
      if (!str) {
        return false;
      }
      out.set(WebIDLValue::makeString(canonical, str));
      return true;
    }
    case ComponentTypeKind::String: {
      const auto& bytes = value.data()->asString().utf8;
      JSString* str = JS_NewStringCopyUTF8N(
          cx, JS::UTF8Chars(bytes.begin(), bytes.length()));
      if (!str) {
        return false;
      }
      out.set(WebIDLValue::makeString(canonical, str));
      return true;
    }
    case ComponentTypeKind::List:
    case ComponentTypeKind::Tuple: {
      RefPtr<WebIDLValueData> data =
          js_new<WebIDLValueData>(WebIDLValueData::SequenceData{});
      if (!data) {
        ReportOutOfMemory(cx);
        return false;
      }
      out.set(WebIDLValue::makeComposite(canonical, data));
      JS::RootedWebIDLValue elemOut(cx);
      for (const ComponentValue& elem : value.data()->asElements().elements) {
        if (!ToCanonicalWebIDLValue(cx, elem, &elemOut, handles) ||
            !data->appendSequenceElement(elemOut.get())) {
          return false;
        }
      }
      return true;
    }
    case ComponentTypeKind::Record: {
      RefPtr<WebIDLValueData> data =
          js_new<WebIDLValueData>(WebIDLValueData::DictionaryData{});
      if (!data) {
        ReportOutOfMemory(cx);
        return false;
      }
      out.set(WebIDLValue::makeComposite(canonical, data));
      JS::RootedWebIDLValue fieldOut(cx);
      for (const ComponentValue& field : value.data()->asElements().elements) {
        if (!ToCanonicalWebIDLValue(cx, field, &fieldOut, handles) ||
            !data->appendDictionaryMember(fieldOut.get())) {
          return false;
        }
      }
      return true;
    }
    case ComponentTypeKind::Flags: {
      RefPtr<WebIDLValueData> data =
          js_new<WebIDLValueData>(WebIDLValueData::DictionaryData{});
      if (!data) {
        ReportOutOfMemory(cx);
        return false;
      }
      out.set(WebIDLValue::makeComposite(canonical, data));
      WebIDLType boolType = WebIDLType::primitive(WebIDLTypeKind::Boolean);
      for (bool bit : value.data()->asFlags().bits) {
        if (!data->appendDictionaryMember(
                WebIDLValue::makeBool(boolType, bit))) {
          return false;
        }
      }
      return true;
    }
    case ComponentTypeKind::Enum:
      out.set(WebIDLValue::makeEnum(canonical, uint32_t(value.asUnsigned())));
      return true;
    case ComponentTypeKind::Option: {
      const auto& option = value.data()->asOption();
      if (ct.asOption().kind() == ComponentTypeKind::Option) {
        // Nested option despecializes to the variant-dict form; see
        // CanonicalWebIDLType. Case 0 = none, case 1 = some.
        return ToCanonicalVariantLikeValue(cx, canonical,
                                           option.payload.isSome() ? 1 : 0,
                                           option.payload, out, handles);
      }
      if (option.payload.isNothing()) {
        out.set(WebIDLValue::makeNull(canonical));
        return true;
      }
      // A present option is stored with its inner (non-nullable) type.
      return ToCanonicalWebIDLValue(cx, *option.payload, out, handles);
    }
    case ComponentTypeKind::Variant:
    case ComponentTypeKind::Result:
      return ToCanonicalVariantLikeValue(
          cx, canonical, VariantLikeCaseIndex(value), VariantLikePayload(value),
          out, handles);
    case ComponentTypeKind::Own:
    case ComponentTypeKind::Borrow: {
      if (!handles) {
        return ReportBridgeUnsupported(
            cx, "an own/borrow handle without a handle table");
      }
      uint32_t index = uint32_t(value.asUnsigned());
      ComponentType resourceType =
          ct.kind() == ComponentTypeKind::Own ? ct.asOwn() : ct.asBorrow();
      JSObject* object;
      if (ct.kind() == ComponentTypeKind::Own) {
        ComponentResourceHandle rawHandle;
        if (!handles->remove(cx, index, &rawHandle)) {
          return false;
        }
        ComponentResourceHandleOwner handle(rawHandle);
        if (handle->resourceType != ComponentResourceTypeId(resourceType)) {
          JS_ReportErrorASCII(cx, "component resource handle type mismatch");
          return false;
        }
        if (!handle->own) {
          JS_ReportErrorASCII(cx,
                              "expected an owning component resource handle");
          return false;
        }
        if (handle->numLends != 0) {
          JS_ReportErrorASCII(
              cx, "component resource handle still has active borrows");
          return false;
        }
        // Uses reflectToJS, not the wrapped object directly: a Native-repr
        // handle has no stored JSObject* and must escape via its interface's
        // reflect op instead.
        object = handle->reflectToJS(cx);
      } else {
        ComponentResourceHandle handle;
        if (!handles->get(cx, index, &handle)) {
          return false;
        }
        if (handle.resourceType != ComponentResourceTypeId(resourceType)) {
          JS_ReportErrorASCII(cx, "component resource handle type mismatch");
          return false;
        }
        object = handle.reflectToJS(cx);
      }
      if (!object) {
        JS_ReportErrorASCII(
            cx, "component resource handle has no way to reflect to JS");
        return false;
      }
      out.set(WebIDLValue::makeObject(canonical, object));
      return true;
    }
    default:
      return ReportBridgeUnsupported(cx, "this component value");
  }
}

static bool FirstCodePoint(JSContext* cx, JSString* str, uint32_t* out) {
  JSLinearString* linear = str->ensureLinear(cx);
  if (!linear) {
    return false;
  }
  if (linear->length() == 0) {
    *out = 0;
    return true;
  }
  char16_t c0 = linear->latin1OrTwoByteChar(0);
  if (c0 >= 0xD800 && c0 <= 0xDBFF && linear->length() >= 2) {
    char16_t c1 = linear->latin1OrTwoByteChar(1);
    if (c1 >= 0xDC00 && c1 <= 0xDFFF) {
      *out = 0x10000 + ((c0 - 0xD800) << 10) + (c1 - 0xDC00);
      return true;
    }
  }
  *out = c0;
  return true;
}

bool js::wasm::FromCanonicalWebIDLValue(JSContext* cx,
                                        JS::HandleWebIDLValue value,
                                        const ComponentType& targetType,
                                        ComponentValue* out,
                                        ComponentHandleTable* handles) {
  switch (targetType.kind()) {
    case ComponentTypeKind::Bool:
      *out = ComponentValue::makeBool(targetType, value.get().asBool());
      return true;
    case ComponentTypeKind::S8:
    case ComponentTypeKind::S16:
    case ComponentTypeKind::S32:
    case ComponentTypeKind::S64:
      *out = ComponentValue::makeSigned(targetType, value.get().asSigned());
      return true;
    case ComponentTypeKind::U8:
    case ComponentTypeKind::U16:
    case ComponentTypeKind::U32:
    case ComponentTypeKind::U64:
      *out = ComponentValue::makeUnsigned(targetType, value.get().asUnsigned());
      return true;
    case ComponentTypeKind::F32:
      *out = ComponentValue::makeF32(targetType, float(value.get().asDouble()));
      return true;
    case ComponentTypeKind::F64:
      *out = ComponentValue::makeF64(targetType, value.get().asDouble());
      return true;
    case ComponentTypeKind::Char: {
      uint32_t cp;
      if (!FirstCodePoint(cx, value.get().asString(), &cp)) {
        return false;
      }
      *out = ComponentValue::makeUnsigned(targetType, cp);
      return true;
    }
    case ComponentTypeKind::String: {
      JS::RootedString str(cx, value.get().asString());
      JS::UniqueChars utf8 = JS_EncodeStringToUTF8(cx, str);
      if (!utf8) {
        return false;
      }
      ComponentValueData::StringData sd;
      size_t len = std::strlen(utf8.get());
      if (!sd.utf8.append(utf8.get(), len)) {
        ReportOutOfMemory(cx);
        return false;
      }
      RefPtr<ComponentValueData> data =
          js_new<ComponentValueData>(std::move(sd));
      if (!data) {
        ReportOutOfMemory(cx);
        return false;
      }
      *out = ComponentValue::makeComposite(targetType, std::move(data));
      return true;
    }
    case ComponentTypeKind::List: {
      ComponentType elementType = targetType.asList();
      ComponentValueData::ElementsData ed;
      const auto& elements = value.get().data()->asSequence().elements;
      JS::RootedWebIDLValue elem(cx);
      for (size_t i = 0; i < elements.length(); i++) {
        elem = elements[i];
        ComponentValue cv;
        if (!FromCanonicalWebIDLValue(cx, elem, elementType, &cv, handles) ||
            !ed.elements.append(cv)) {
          return false;
        }
      }
      RefPtr<ComponentValueData> data =
          js_new<ComponentValueData>(std::move(ed));
      if (!data) {
        ReportOutOfMemory(cx);
        return false;
      }
      *out = ComponentValue::makeComposite(targetType, std::move(data));
      return true;
    }
    case ComponentTypeKind::Tuple: {
      const ComponentTypeVector& items = targetType.asTuple();
      ComponentValueData::ElementsData ed;
      const auto& elements = value.get().data()->asSequence().elements;
      JS::RootedWebIDLValue elem(cx);
      for (size_t i = 0; i < items.length() && i < elements.length(); i++) {
        elem = elements[i];
        ComponentValue cv;
        if (!FromCanonicalWebIDLValue(cx, elem, items[i], &cv, handles) ||
            !ed.elements.append(cv)) {
          return false;
        }
      }
      RefPtr<ComponentValueData> data =
          js_new<ComponentValueData>(std::move(ed));
      if (!data) {
        ReportOutOfMemory(cx);
        return false;
      }
      *out = ComponentValue::makeComposite(targetType, std::move(data));
      return true;
    }
    case ComponentTypeKind::Record: {
      const ComponentRecordFieldVector& fields = targetType.asRecord();
      ComponentValueData::ElementsData ed;
      const auto& members = value.get().data()->asDictionary().members;
      JS::RootedWebIDLValue member(cx);
      for (size_t i = 0; i < fields.length() && i < members.length(); i++) {
        member = members[i];
        ComponentValue cv;
        if (!FromCanonicalWebIDLValue(cx, member, fields[i].type, &cv,
                                      handles) ||
            !ed.elements.append(cv)) {
          return false;
        }
      }
      RefPtr<ComponentValueData> data =
          js_new<ComponentValueData>(std::move(ed));
      if (!data) {
        ReportOutOfMemory(cx);
        return false;
      }
      *out = ComponentValue::makeComposite(targetType, std::move(data));
      return true;
    }
    case ComponentTypeKind::Flags: {
      const CacheableNameVector& labels = targetType.asFlags();
      ComponentValueData::FlagsData fd;
      const auto& members = value.get().data()->asDictionary().members;
      for (size_t i = 0; i < labels.length() && i < members.length(); i++) {
        if (!fd.bits.append(members[i].asBool())) {
          ReportOutOfMemory(cx);
          return false;
        }
      }
      RefPtr<ComponentValueData> data =
          js_new<ComponentValueData>(std::move(fd));
      if (!data) {
        ReportOutOfMemory(cx);
        return false;
      }
      *out = ComponentValue::makeComposite(targetType, std::move(data));
      return true;
    }
    case ComponentTypeKind::Enum:
      *out =
          ComponentValue::makeUnsigned(targetType, value.get().asEnumIndex());
      return true;
    case ComponentTypeKind::Option: {
      ComponentType inner = targetType.asOption();
      ComponentValueData::OptionData od;
      if (inner.kind() == ComponentTypeKind::Option) {
        // Nested option despecializes to the variant-dict form; see
        // CanonicalWebIDLType. Case 0 = none, case 1 = some.
        const JS::WebIDLValueVector& members =
            value.get().data()->asDictionary().members;
        if (members[0].asEnumIndex() == 1) {
          JS::RootedWebIDLValue memberVal(cx, members[1]);
          ComponentValue cv;
          if (!FromCanonicalWebIDLValue(cx, memberVal, inner, &cv, handles)) {
            return false;
          }
          od.payload = mozilla::Some(cv);
        }
      } else if (!(value.get().type().kind() == JS::WebIDLTypeKind::Nullable &&
                   value.get().isNull())) {
        ComponentValue innerVal;
        if (!FromCanonicalWebIDLValue(cx, value, inner, &innerVal, handles)) {
          return false;
        }
        od.payload = mozilla::Some(innerVal);
      }
      RefPtr<ComponentValueData> data =
          js_new<ComponentValueData>(std::move(od));
      if (!data) {
        ReportOutOfMemory(cx);
        return false;
      }
      *out = ComponentValue::makeComposite(targetType, std::move(data));
      return true;
    }
    case ComponentTypeKind::Variant:
    case ComponentTypeKind::Result: {
      const JS::WebIDLValueVector& members =
          value.get().data()->asDictionary().members;
      uint32_t caseIndex = members[0].asEnumIndex();
      mozilla::Maybe<ComponentType> caseType =
          VariantLikeCaseType(targetType, caseIndex);
      mozilla::Maybe<ComponentValue> payload;
      if (caseType) {
        JS::RootedWebIDLValue memberVal(cx, members[1]);
        ComponentValue cv;
        if (!FromCanonicalWebIDLValue(cx, memberVal, *caseType, &cv, handles)) {
          return false;
        }
        payload = mozilla::Some(cv);
      }
      return MakeVariantLikeValue(cx, targetType, caseIndex, std::move(payload),
                                  out);
    }
    case ComponentTypeKind::Own:
    case ComponentTypeKind::Borrow: {
      if (!handles) {
        return ReportBridgeUnsupported(
            cx, "an own/borrow handle without a handle table");
      }
      // A resource canonicalizes to Object or Interface - either way, the
      // value's repr is a plain JSObject* (see JSToWebIDLValue's
      // Object/Interface cases).
      JS::WebIDLTypeKind valueKind = value.get().type().kind();
      if ((valueKind != JS::WebIDLTypeKind::Object &&
           valueKind != JS::WebIDLTypeKind::Interface) ||
          !value.get().asObject()) {
        JS_ReportErrorASCII(
            cx,
            "expected a WebIDL object value for a component resource "
            "handle");
        return false;
      }
      bool own = targetType.kind() == ComponentTypeKind::Own;
      ComponentType resourceType =
          own ? targetType.asOwn() : targetType.asBorrow();
      JSObject* obj = value.get().asObject();
      JS::WebIDLInterface* iface =
          (resourceType.kind() == ComponentTypeKind::Resource ||
           resourceType.kind() == ComponentTypeKind::SubResource)
              ? resourceType.asResource().interface()
              : nullptr;
      // Uses same-compartment identity as a same-origin proxy stand-in for this
      // prototype; real origin/principal checks are out of scope.
      bool isSameOrigin = obj->compartment() == cx->compartment();
      ComponentResourceClassification classification =
          ClassifyResourceObject(obj, isSameOrigin, iface);
      uint32_t index;
      if (!handles->add(cx, ComponentResourceTypeId(resourceType), own,
                        classification, &index)) {
        return false;
      }
      *out = ComponentValue::makeUnsigned(targetType, index);
      return true;
    }
    default:
      return ReportBridgeUnsupported(cx, "this target component type");
  }
}

// Deliberately duplicated versions of WebIDLConvert.cpp's
// IntegerWidthAndSign/IntegerBounds: this file needs the component-side
// integer widths, and sharing the logic isn't worth it for two small
// switches.

static void ComponentIntegerWidthAndSign(ComponentTypeKind kind, uint8_t* bits,
                                         bool* isSigned) {
  switch (kind) {
    case ComponentTypeKind::S8:
      *bits = 8;
      *isSigned = true;
      break;
    case ComponentTypeKind::U8:
      *bits = 8;
      *isSigned = false;
      break;
    case ComponentTypeKind::S16:
      *bits = 16;
      *isSigned = true;
      break;
    case ComponentTypeKind::U16:
      *bits = 16;
      *isSigned = false;
      break;
    case ComponentTypeKind::S32:
      *bits = 32;
      *isSigned = true;
      break;
    case ComponentTypeKind::U32:
      *bits = 32;
      *isSigned = false;
      break;
    case ComponentTypeKind::S64:
      *bits = 64;
      *isSigned = true;
      break;
    case ComponentTypeKind::U64:
      *bits = 64;
      *isSigned = false;
      break;
    default:
      MOZ_CRASH("not a component integer kind");
  }
}

static bool ComponentTypeKindIsInteger(ComponentTypeKind kind) {
  switch (kind) {
    case ComponentTypeKind::S8:
    case ComponentTypeKind::U8:
    case ComponentTypeKind::S16:
    case ComponentTypeKind::U16:
    case ComponentTypeKind::S32:
    case ComponentTypeKind::U32:
    case ComponentTypeKind::S64:
    case ComponentTypeKind::U64:
      return true;
    default:
      return false;
  }
}

static void WebIDLIntegerWidthAndSign(JS::WebIDLTypeKind kind, uint8_t* bits,
                                      bool* isSigned) {
  switch (kind) {
    case JS::WebIDLTypeKind::Byte:
      *bits = 8;
      *isSigned = true;
      break;
    case JS::WebIDLTypeKind::Octet:
      *bits = 8;
      *isSigned = false;
      break;
    case JS::WebIDLTypeKind::Short:
      *bits = 16;
      *isSigned = true;
      break;
    case JS::WebIDLTypeKind::UnsignedShort:
      *bits = 16;
      *isSigned = false;
      break;
    case JS::WebIDLTypeKind::Long:
      *bits = 32;
      *isSigned = true;
      break;
    case JS::WebIDLTypeKind::UnsignedLong:
      *bits = 32;
      *isSigned = false;
      break;
    case JS::WebIDLTypeKind::LongLong:
      *bits = 64;
      *isSigned = true;
      break;
    case JS::WebIDLTypeKind::UnsignedLongLong:
      *bits = 64;
      *isSigned = false;
      break;
    default:
      MOZ_CRASH("not a WebIDL integer kind");
  }
}

// Unsigned 64-bit's true max (2^64-1) doesn't fit in int64_t; this saturates
// to INT64_MAX as a known, deliberately-unfixed approximation.
static void IntegerRange(uint8_t bits, bool isSigned, int64_t* min,
                         int64_t* max) {
  if (bits == 64) {
    *min = isSigned ? INT64_MIN : 0;
    *max = INT64_MAX;
    return;
  }
  if (isSigned) {
    *min = -(int64_t(1) << (bits - 1));
    *max = (int64_t(1) << (bits - 1)) - 1;
  } else {
    *min = 0;
    *max = (int64_t(1) << bits) - 1;
  }
}

static void ChooseIntStep(ComponentTypeKind componentKind,
                          const JS::WebIDLType& target, CoerceStep* out) {
  uint8_t srcBits;
  bool srcSigned;
  ComponentIntegerWidthAndSign(componentKind, &srcBits, &srcSigned);
  int64_t srcMin, srcMax;
  IntegerRange(srcBits, srcSigned, &srcMin, &srcMax);

  uint8_t targetBits;
  bool targetSigned;
  WebIDLIntegerWidthAndSign(target.kind(), &targetBits, &targetSigned);
  int64_t targetMin, targetMax;
  IntegerRange(targetBits, targetSigned, &targetMin, &targetMax);

  out->srcBits = srcBits;
  out->srcSigned = srcSigned;
  out->targetBits = targetBits;
  out->targetSigned = targetSigned;
  out->rangeMin = targetMin;
  out->rangeMax = targetMax;

  if (srcMin >= targetMin && srcMax <= targetMax) {
    out->op = srcSigned ? CoerceOp::IntWidenSigned : CoerceOp::IntWidenUnsigned;
  } else if (target.hasEnforceRange()) {
    out->op = CoerceOp::IntEnforceRange;
  } else if (target.hasClamp()) {
    out->op = CoerceOp::IntClamp;
  } else {
    out->op = CoerceOp::IntModular;
  }
}

static void ChooseFloatStep(const JS::WebIDLType& canonical,
                            ComponentTypeKind componentKind,
                            const JS::WebIDLType& target, CoerceStep* out) {
  bool restricted = target.kind() == JS::WebIDLTypeKind::Float ||
                    target.kind() == JS::WebIDLTypeKind::Double;
  bool targetIs64 = target.kind() == JS::WebIDLTypeKind::Double ||
                    target.kind() == JS::WebIDLTypeKind::UnrestrictedDouble;
  out->targetBits = targetIs64 ? 64 : 32;
  out->restrictFloat = restricted;

  if (JS::WebIDLTypeKindIsInteger(canonical.kind())) {
    uint8_t srcBits;
    bool srcSigned;
    ComponentIntegerWidthAndSign(componentKind, &srcBits, &srcSigned);
    out->srcBits = srcBits;
    out->srcSigned = srcSigned;
    out->op =
        srcSigned ? CoerceOp::FloatFromSigned : CoerceOp::FloatFromUnsigned;
    return;
  }
  bool srcIs64 = componentKind == ComponentTypeKind::F64;
  out->srcBits = srcIs64 ? 64 : 32;
  if (!srcIs64 && targetIs64) {
    out->op = CoerceOp::F32ToF64;
  } else if (srcIs64 && !targetIs64) {
    out->op = CoerceOp::F64ToF32;
  } else {
    out->op = CoerceOp::FloatIdentity;
  }
}

// Maps a WebIDL string type kind to the CoerceStep-level WebIDLStringKind
// tag: WasmComponent.h's alias for JS::WebIDLStringKind, which distinguishes
// how a String-kind step encodes its bytes.
static WebIDLStringKind StringKindFromWebIDLKind(JS::WebIDLTypeKind kind) {
  if (kind == JS::WebIDLTypeKind::DOMString) {
    return WebIDLStringKind::DOMString;
  }
  if (kind == JS::WebIDLTypeKind::ByteString) {
    return WebIDLStringKind::ByteString;
  }
  return WebIDLStringKind::USVString;
}

// Counterparts to StoreElements/StoreRecord/FlattenType that compute where a
// given element/field would be, without needing actual values.

static const ComponentType& TypeAt(const ComponentTypeVector& types,
                                   size_t index) {
  return types[index];
}
static const ComponentType& FieldTypeAt(
    const ComponentRecordFieldVector& fields, size_t index) {
  return fields[index].type;
}

template <typename Container>
using ComponentTypeAtFn = const ComponentType& (*)(const Container&, size_t);

template <typename Container>
static uint32_t ElemOffsetByType(const Container& container, size_t index,
                                 ComponentTypeAtFn<Container> typeAt) {
  uint32_t p = 0;
  for (size_t i = 0; i < index; i++) {
    p = AlignTo(p, Alignment(typeAt(container, i)));
    p += ElemSize(typeAt(container, i));
  }
  return AlignTo(p, Alignment(typeAt(container, index)));
}

static uint32_t ElemOffset(const ComponentTypeVector& types, size_t index) {
  return ElemOffsetByType(types, index, TypeAt);
}

static uint32_t RecordFieldOffset(const ComponentRecordFieldVector& fields,
                                  size_t index) {
  return ElemOffsetByType(fields, index, FieldTypeAt);
}

// The flat-slot offset at which typeAt(container, index)'s own FlattenType
// representation starts. Record/Tuple flatten transparently and recurse the
// same way FlattenType does. Not valid for a Variant/Option/Result element;
// callers must not reach here for those.
template <typename Container>
static bool FlatOffsetByType(const Container& container, size_t index,
                             ComponentTypeAtFn<Container> typeAt,
                             uint32_t* out) {
  uint32_t slot = 0;
  for (size_t i = 0; i < index; i++) {
    ValTypeVector flat;
    if (!FlattenType(typeAt(container, i), &flat)) {
      return false;
    }
    slot += flat.length();
  }
  *out = slot;
  return true;
}

static bool FlatOffsetInTypes(const ComponentTypeVector& types, size_t index,
                              uint32_t* out) {
  return FlatOffsetByType(types, index, TypeAt, out);
}

static bool FlatOffsetInFields(const ComponentRecordFieldVector& fields,
                               size_t index, uint32_t* out) {
  return FlatOffsetByType(fields, index, FieldTypeAt, out);
}

// Where a CoerceStep's own top-level data is read from, threaded through the
// BuildCoerceStep recursion. Distinct from the public AccessLoc, which
// BuildCoerceStep derives from this plus a node's own base+mode.
struct BuildContext {
  enum class Kind : uint8_t { Flat, Mem } kind;
  uint32_t base;
};

static AccessLoc AccessFromContext(const BuildContext& ctx) {
  AccessLoc access;
  if (ctx.kind == BuildContext::Kind::Flat) {
    access.mode = AccessLoc::Mode::FlatSlot;
    access.flatSlot = ctx.base;
  } else {
    access.mode = AccessLoc::Mode::Mem;
    access.offset = ctx.base;
  }
  return access;
}

static bool BuildEnumRemap(
    JSContext* cx, const CacheableNameVector& sourceLabels,
    const JS::WebIDLEnumerationType& targetEnum,
    mozilla::Vector<uint32_t, 0, SystemAllocPolicy>* out) {
  if (!out->reserve(sourceLabels.length())) {
    ReportOutOfMemory(cx);
    return false;
  }
  for (const CacheableName& label : sourceLabels) {
    mozilla::Span<const char> bytes = label.utf8Bytes();
    mozilla::Maybe<uint32_t> targetIndex =
        targetEnum.find(bytes.data(), bytes.Length());
    out->infallibleAppend(targetIndex ? *targetIndex
                                      : CoerceStep::kNoEnumMatch);
  }
  return true;
}

static bool FindComponentRecordField(const ComponentRecordFieldVector& fields,
                                     const char* name, size_t nameLen,
                                     size_t* outIndex) {
  for (size_t i = 0; i < fields.length(); i++) {
    mozilla::Span<const char> fieldName = fields[i].name.utf8Bytes();
    if (fieldName.Length() == nameLen &&
        memcmp(fieldName.data(), name, nameLen) == 0) {
      *outIndex = i;
      return true;
    }
  }
  return false;
}

static bool BuildCoerceStep(JSContext* cx, const ComponentType& componentType,
                            const BuildContext& ctx,
                            const JS::WebIDLType& target,
                            ComponentStringEncoding encoding, CoerceStep* out);

// A Dictionary target is only sourced from a component `record`; a `flags`
// source needing bit-extraction into a dictionary-of-booleans is not modeled
// and traps unconditionally. A required member that's unconditionally missing
// collapses the whole Dictionary step to Trap, since dictionary member
// resolution doesn't depend on a runtime value the way a Sequence's elements
// do.
static bool BuildDictionaryStep(JSContext* cx,
                                const ComponentType& componentType,
                                const BuildContext& ctx,
                                const JS::WebIDLType& target,
                                ComponentStringEncoding encoding,
                                CoerceStep* out) {
  out->kind = StepKind::Dictionary;

  if (componentType.kind() == ComponentTypeKind::Flags) {
    out->kind = StepKind::Trap;
    return true;
  }

  const JS::WebIDLDictionaryMemberVector& members =
      target.def()->schema().as<JS::WebIDLDictionaryType>().members;
  bool isRecordSource = componentType.kind() == ComponentTypeKind::Record;
  const ComponentRecordFieldVector* fields =
      isRecordSource ? &componentType.asRecord() : nullptr;

  if (!out->dictMembers.reserve(members.length())) {
    ReportOutOfMemory(cx);
    return false;
  }
  bool doomed = false;
  for (const JS::WebIDLDictionaryMember& m : members) {
    DictMemberStep dm;
    size_t fieldIndex = 0;
    bool found =
        fields && FindComponentRecordField(*fields, m.name.get(),
                                           strlen(m.name.get()), &fieldIndex);
    if (found) {
      dm.presence = DictMemberStep::Presence::FromSource;
      dm.sourceField = fieldIndex;
      dm.step = mozilla::MakeUnique<CoerceStep>();
      if (!dm.step) {
        ReportOutOfMemory(cx);
        return false;
      }
      BuildContext fieldCtx;
      if (ctx.kind == BuildContext::Kind::Flat) {
        uint32_t off;
        if (!FlatOffsetInFields(*fields, fieldIndex, &off)) {
          ReportOutOfMemory(cx);
          return false;
        }
        fieldCtx = {BuildContext::Kind::Flat, ctx.base + off};
      } else {
        fieldCtx = {BuildContext::Kind::Mem,
                    ctx.base + RecordFieldOffset(*fields, fieldIndex)};
      }
      if (!BuildCoerceStep(cx, (*fields)[fieldIndex].type, fieldCtx, m.type,
                           encoding, dm.step.get())) {
        return false;
      }
      if (dm.step->kind == StepKind::Trap) {
        doomed = true;
      }
    } else if (!m.required) {
      dm.presence = DictMemberStep::Presence::Default;
    } else {
      dm.presence = DictMemberStep::Presence::MissingRequired;
      doomed = true;
    }
    if (!out->dictMembers.append(std::move(dm))) {
      ReportOutOfMemory(cx);
      return false;
    }
  }
  if (doomed) {
    out->kind = StepKind::Trap;
    out->dictMembers.clear();
  }
  return true;
}

// A Variant target's source is a component variant/result<T,E>, or a nested
// option<option<T>> redirected here by BuildCoerceStep's Option handling -
// all despecialize to the same Dictionary {kind, value?} shape. The
// discriminant is always structurally readable; a case's payload failing to
// resolve only dooms that case's payloadStep, not the whole binding.
static bool BuildVariantStep(JSContext* cx, const ComponentType& componentType,
                             const BuildContext& ctx,
                             const JS::WebIDLType& target,
                             ComponentStringEncoding encoding,
                             CoerceStep* out) {
  if (target.kind() != JS::WebIDLTypeKind::Dictionary) {
    out->kind = StepKind::Trap;
    return true;
  }
  const JS::WebIDLDictionaryMemberVector& members =
      target.def()->schema().as<JS::WebIDLDictionaryType>().members;
  const JS::WebIDLDictionaryMember* kindMember = nullptr;
  const JS::WebIDLDictionaryMember* valueMember = nullptr;
  for (const JS::WebIDLDictionaryMember& m : members) {
    if (strcmp(m.name.get(), "kind") == 0) {
      kindMember = &m;
    } else if (strcmp(m.name.get(), "value") == 0) {
      valueMember = &m;
    }
  }
  if (!kindMember ||
      kindMember->type.kind() != JS::WebIDLTypeKind::Enumeration) {
    out->kind = StepKind::Trap;
    return true;
  }
  const JS::WebIDLEnumerationType& kindEnum =
      kindMember->type.def()->schema().as<JS::WebIDLEnumerationType>();

  out->kind = StepKind::Variant;
  uint32_t numCases = NumBuildVariantCases(componentType);
  uint32_t discSize = DiscriminantSize(numCases);

  BuildContext payloadCtx;
  if (ctx.kind == BuildContext::Kind::Flat) {
    payloadCtx = {BuildContext::Kind::Flat, ctx.base + 1};
  } else {
    payloadCtx = {BuildContext::Kind::Mem,
                  AlignTo(ctx.base + discSize,
                          BuildVariantMaxPayloadAlign(componentType))};
  }

  if (!out->enumRemap.reserve(numCases) ||
      !out->variantCases.reserve(numCases)) {
    ReportOutOfMemory(cx);
    return false;
  }
  for (uint32_t i = 0; i < numCases; i++) {
    mozilla::Span<const char> label = BuildVariantCaseLabel(componentType, i);
    mozilla::Maybe<uint32_t> targetIndex =
        kindEnum.find(label.data(), label.Length());
    out->enumRemap.infallibleAppend(targetIndex ? *targetIndex
                                                : CoerceStep::kNoEnumMatch);

    VariantCaseStep caseStep;
    mozilla::Maybe<ComponentType> caseType =
        BuildVariantCaseType(componentType, i);
    if (caseType) {
      caseStep.payloadStep = mozilla::MakeUnique<CoerceStep>();
      if (!caseStep.payloadStep) {
        ReportOutOfMemory(cx);
        return false;
      }
      const JS::WebIDLType* payloadTarget = nullptr;
      JS::WebIDLType unionArm;
      if (valueMember &&
          valueMember->type.kind() == JS::WebIDLTypeKind::Union) {
        JS::WebIDLType caseCanonical;
        if (!CanonicalWebIDLType(cx, *caseType, &caseCanonical)) {
          return false;
        }
        const JS::WebIDLTypeVector& arms =
            valueMember->type.def()->schema().as<JS::WebIDLUnionType>().members;
        for (const JS::WebIDLType& arm : arms) {
          if (JS::WebIDLOverloadKind(arm) ==
              JS::WebIDLOverloadKind(caseCanonical)) {
            unionArm = arm;
            payloadTarget = &unionArm;
            break;
          }
        }
      } else if (valueMember) {
        payloadTarget = &valueMember->type;
      }
      if (!payloadTarget) {
        caseStep.payloadStep->kind = StepKind::Trap;
      } else if (!BuildCoerceStep(cx, *caseType, payloadCtx, *payloadTarget,
                                  encoding, caseStep.payloadStep.get())) {
        return false;
      }
    }
    if (!out->variantCases.append(std::move(caseStep))) {
      ReportOutOfMemory(cx);
      return false;
    }
  }
  return true;
}

// Reads componentType (addressed per ctx) and coerces it to target. Always
// succeeds (returns false only on OOM); a statically-doomed coercion becomes
// a Trap-kind step rather than an error.
static bool BuildCoerceStep(JSContext* cx, const ComponentType& componentType,
                            const BuildContext& ctx,
                            const JS::WebIDLType& target,
                            ComponentStringEncoding encoding, CoerceStep* out) {
  *out = CoerceStep();
  out->targetType = target;
  out->access = AccessFromContext(ctx);

  ComponentTypeKind srcKind = componentType.kind();

  // Any/Object always fall back (at execution time) to a generic lift plus the
  // existing oracle path, so this must run before the exclusion below: a
  // variant/option/result source reaching Any/Object isn't doomed.
  if (target.kind() == JS::WebIDLTypeKind::Any) {
    out->kind = StepKind::Any;
    out->genericSourceTypeKind = componentType.kind();
    out->genericSourceTypeDef = componentType.typeDef();
    return true;
  }
  if (target.kind() == JS::WebIDLTypeKind::Object) {
    out->kind = StepKind::Object;
    out->genericSourceTypeKind = componentType.kind();
    out->genericSourceTypeDef = componentType.typeDef();
    return true;
  }

  // Option source: read the discriminant, then produce null or recurse into
  // the payload. Must run before the generic Nullable case below, which would
  // otherwise recurse endlessly for a genuinely-optional source.
  if (srcKind == ComponentTypeKind::Option) {
    ComponentType inner = componentType.asOption();
    if (inner.kind() != ComponentTypeKind::Option) {
      if (target.kind() != JS::WebIDLTypeKind::Nullable) {
        out->kind = StepKind::Trap;
        return true;
      }
      out->kind = StepKind::Option;
      out->child = mozilla::MakeUnique<CoerceStep>();
      if (!out->child) {
        ReportOutOfMemory(cx);
        return false;
      }
      BuildContext childCtx;
      if (ctx.kind == BuildContext::Kind::Flat) {
        childCtx = {BuildContext::Kind::Flat, ctx.base + 1};
      } else {
        uint32_t discSize = DiscriminantSize(2);
        childCtx = {BuildContext::Kind::Mem,
                    AlignTo(ctx.base + discSize, Alignment(inner))};
      }
      if (!BuildCoerceStep(cx, inner, childCtx, target.innerType(), encoding,
                           out->child.get())) {
        return false;
      }
      if (out->child->kind == StepKind::Trap) {
        out->kind = StepKind::Trap;
        out->child = nullptr;
      }
      return true;
    }
  }

  // Variant/result sources, and a nested option<option<T>>, all despecialize to
  // the same WebIDL Dictionary {kind, value?} shape.
  if (srcKind == ComponentTypeKind::Variant ||
      srcKind == ComponentTypeKind::Result ||
      srcKind == ComponentTypeKind::Option) {
    return BuildVariantStep(cx, componentType, ctx, target, encoding, out);
  }

  // Component `char` (a flat i32 Unicode scalar) takes the Scalar path here,
  // not String - there's no (ptr,len) in memory to read.
  if (srcKind == ComponentTypeKind::Char &&
      JS::WebIDLTypeKindIsString(target.kind())) {
    out->kind = StepKind::Scalar;
    out->op = CoerceOp::CharToString;
    out->targetString = StringKindFromWebIDLKind(target.kind());
    return true;
  }

  JS::WebIDLType canonical;
  if (!CanonicalWebIDLType(cx, componentType, &canonical)) {
    return false;
  }

  // Compound targets are exempt from the class-match guard: Union's class is a
  // set, Nullable includes Null, Any/Object accept anything.
  JS::WebIDLTypeKind targetKind = target.kind();
  bool exempt = targetKind == JS::WebIDLTypeKind::Any ||
                targetKind == JS::WebIDLTypeKind::Object ||
                targetKind == JS::WebIDLTypeKind::Union ||
                targetKind == JS::WebIDLTypeKind::Nullable;
  if (!exempt &&
      JS::WebIDLOverloadKind(canonical) != JS::WebIDLOverloadKind(target)) {
    out->kind = StepKind::Trap;
    return true;
  }

  switch (targetKind) {
    case JS::WebIDLTypeKind::Boolean:
      out->kind = StepKind::Scalar;
      out->op = CoerceOp::BoolIdentity;
      return true;

    case JS::WebIDLTypeKind::Byte:
    case JS::WebIDLTypeKind::Octet:
    case JS::WebIDLTypeKind::Short:
    case JS::WebIDLTypeKind::UnsignedShort:
    case JS::WebIDLTypeKind::Long:
    case JS::WebIDLTypeKind::UnsignedLong:
    case JS::WebIDLTypeKind::LongLong:
    case JS::WebIDLTypeKind::UnsignedLongLong:
      if (!ComponentTypeKindIsInteger(srcKind)) {
        out->kind = StepKind::Trap;
        return true;
      }
      out->kind = StepKind::Scalar;
      ChooseIntStep(srcKind, target, out);
      return true;

    case JS::WebIDLTypeKind::Float:
    case JS::WebIDLTypeKind::UnrestrictedFloat:
    case JS::WebIDLTypeKind::Double:
    case JS::WebIDLTypeKind::UnrestrictedDouble:
      if (!ComponentTypeKindIsInteger(srcKind) &&
          srcKind != ComponentTypeKind::F32 &&
          srcKind != ComponentTypeKind::F64) {
        out->kind = StepKind::Trap;
        return true;
      }
      out->kind = StepKind::Scalar;
      ChooseFloatStep(canonical, srcKind, target, out);
      return true;

    case JS::WebIDLTypeKind::BigInt: {
      if (!ComponentTypeKindIsInteger(srcKind)) {
        out->kind = StepKind::Trap;
        return true;
      }
      uint8_t srcBits;
      bool srcSigned;
      ComponentIntegerWidthAndSign(srcKind, &srcBits, &srcSigned);
      out->kind = StepKind::Scalar;
      out->srcBits = srcBits;
      out->srcSigned = srcSigned;
      out->op =
          srcSigned ? CoerceOp::BigIntFromSigned : CoerceOp::BigIntFromUnsigned;
      return true;
    }

    case JS::WebIDLTypeKind::DOMString:
    case JS::WebIDLTypeKind::USVString:
    case JS::WebIDLTypeKind::ByteString:
      out->kind = StepKind::String;
      out->srcEncoding = encoding;
      out->targetString = StringKindFromWebIDLKind(targetKind);
      return true;

    case JS::WebIDLTypeKind::Enumeration: {
      if (srcKind != ComponentTypeKind::Enum) {
        out->kind = StepKind::Trap;
        return true;
      }
      out->kind = StepKind::Enumeration;
      if (!BuildEnumRemap(
              cx, componentType.asEnum(),
              target.def()->schema().as<JS::WebIDLEnumerationType>(),
              &out->enumRemap)) {
        return false;
      }
      return true;
    }

    case JS::WebIDLTypeKind::Interface: {
      if (srcKind != ComponentTypeKind::Own &&
          srcKind != ComponentTypeKind::Borrow) {
        out->kind = StepKind::Trap;
        return true;
      }
      bool srcOwn = srcKind == ComponentTypeKind::Own;
      ComponentType resourceType =
          srcOwn ? componentType.asOwn() : componentType.asBorrow();
      // Uses the resource's statically declared interface, not dynamic
      // most-derived identity (which would need a reflector or virtual hook). A
      // resource with no declared interface (including an imported
      // (SubResource) one whose import didn't supply one) can never satisfy a
      // specific Interface target.
      JS::WebIDLInterface* srcIface =
          (resourceType.kind() == ComponentTypeKind::Resource ||
           resourceType.kind() == ComponentTypeKind::SubResource)
              ? resourceType.asResource().interface()
              : nullptr;
      if (!srcIface || !srcIface->isOrDerivedFrom(target.interface())) {
        out->kind = StepKind::Trap;
        return true;
      }
      out->kind = StepKind::Interface;
      out->expectedInterface = target.interface();
      out->resourceOwn = srcOwn;
      out->resourceTypeKind = resourceType.kind();
      out->resourceTypeDef = resourceType.typeDef();
      return true;
    }

    case JS::WebIDLTypeKind::Nullable: {
      out->kind = StepKind::Nullable;
      out->child = mozilla::MakeUnique<CoerceStep>();
      if (!out->child) {
        ReportOutOfMemory(cx);
        return false;
      }
      if (!BuildCoerceStep(cx, componentType, ctx, target.innerType(), encoding,
                           out->child.get())) {
        return false;
      }
      if (out->child->kind == StepKind::Trap) {
        out->kind = StepKind::Trap;
        out->child = nullptr;
      }
      return true;
    }

    case JS::WebIDLTypeKind::Sequence:
    case JS::WebIDLTypeKind::FrozenArray:
    case JS::WebIDLTypeKind::ObservableArray: {
      if (srcKind != ComponentTypeKind::List) {
        out->kind = StepKind::Trap;
        return true;
      }
      ComponentType elemType = componentType.asList();
      out->kind = StepKind::Sequence;
      out->access.elemStride = ElemSize(elemType);
      out->child = mozilla::MakeUnique<CoerceStep>();
      if (!out->child) {
        ReportOutOfMemory(cx);
        return false;
      }
      BuildContext elemCtx{BuildContext::Kind::Mem, 0};
      if (!BuildCoerceStep(cx, elemType, elemCtx, target.innerType(), encoding,
                           out->child.get())) {
        return false;
      }
      // Not propagated to Trap: an empty list never reaches the per-element
      // step, but that depends on a runtime value (the list's length), unlike
      // the Dictionary/Nullable/Union cases above.
      return true;
    }

    case JS::WebIDLTypeKind::Dictionary:
      return BuildDictionaryStep(cx, componentType, ctx, target, encoding, out);

    case JS::WebIDLTypeKind::Union: {
      const JS::WebIDLTypeVector& arms =
          target.def()->schema().as<JS::WebIDLUnionType>().members;
      const JS::WebIDLType* arm = nullptr;
      for (const JS::WebIDLType& candidate : arms) {
        if (JS::WebIDLOverloadKind(candidate) ==
            JS::WebIDLOverloadKind(canonical)) {
          arm = &candidate;
          break;
        }
      }
      if (!arm) {
        out->kind = StepKind::Trap;
        return true;
      }
      out->kind = StepKind::Union;
      out->child = mozilla::MakeUnique<CoerceStep>();
      if (!out->child) {
        ReportOutOfMemory(cx);
        return false;
      }
      if (!BuildCoerceStep(cx, componentType, ctx, *arm, encoding,
                           out->child.get())) {
        return false;
      }
      if (out->child->kind == StepKind::Trap) {
        out->kind = StepKind::Trap;
        out->child = nullptr;
      }
      return true;
    }

    case JS::WebIDLTypeKind::BufferSource: {
      // Only a component `list<u8>` bridges to a buffer source, and only to a
      // byte-typed one: the conversion is a bulk memcpy of the list's element
      // buffer, which needs both sides to agree that an element is one byte.
      // A generic `ArrayBufferView` target has no statically known element
      // type, so it traps like any other unsupported shape.
      if (srcKind != ComponentTypeKind::List ||
          !JS::WebIDLBufferSourceKindIsBytes(target.bufferSourceKind()) ||
          componentType.asList().kind() != ComponentTypeKind::U8) {
        out->kind = StepKind::Trap;
        return true;
      }
      out->kind = StepKind::BufferSource;
      out->bufferKind = target.bufferSourceKind();
      out->access.elemStride = 1;
      return true;
    }

    // A WebIDL record<K,V> target is underspecified for a fixed-shape component
    // source and not modeled, same as Promise/ReadableStream/Callback.
    case JS::WebIDLTypeKind::Record:
    case JS::WebIDLTypeKind::Promise:
    case JS::WebIDLTypeKind::ReadableStream:
    case JS::WebIDLTypeKind::Callback:
    default:
      out->kind = StepKind::Trap;
      return true;
  }
}

// Name helpers for MOZ_LOG=wasmComponents:3 diagnostics only (see
// BuildCallAdapter and ReportComponentTrap below) - not used for any
// user-facing error message.
static const char* ComponentTypeKindName(ComponentTypeKind kind) {
  switch (kind) {
    case ComponentTypeKind::Invalid:
      return "invalid";
    case ComponentTypeKind::Bool:
      return "bool";
    case ComponentTypeKind::S8:
      return "s8";
    case ComponentTypeKind::U8:
      return "u8";
    case ComponentTypeKind::S16:
      return "s16";
    case ComponentTypeKind::U16:
      return "u16";
    case ComponentTypeKind::S32:
      return "s32";
    case ComponentTypeKind::U32:
      return "u32";
    case ComponentTypeKind::S64:
      return "s64";
    case ComponentTypeKind::U64:
      return "u64";
    case ComponentTypeKind::F32:
      return "f32";
    case ComponentTypeKind::F64:
      return "f64";
    case ComponentTypeKind::Char:
      return "char";
    case ComponentTypeKind::String:
      return "string";
    case ComponentTypeKind::Record:
      return "record";
    case ComponentTypeKind::Variant:
      return "variant";
    case ComponentTypeKind::List:
      return "list";
    case ComponentTypeKind::Tuple:
      return "tuple";
    case ComponentTypeKind::Flags:
      return "flags";
    case ComponentTypeKind::Enum:
      return "enum";
    case ComponentTypeKind::Option:
      return "option";
    case ComponentTypeKind::Result:
      return "result";
    case ComponentTypeKind::Own:
      return "own";
    case ComponentTypeKind::Borrow:
      return "borrow";
    case ComponentTypeKind::Func:
      return "func";
    case ComponentTypeKind::Component:
      return "component";
    case ComponentTypeKind::Instance:
      return "instance";
    case ComponentTypeKind::Resource:
      return "resource";
    case ComponentTypeKind::Eq:
      return "eq";
    case ComponentTypeKind::SubResource:
      return "sub-resource";
    default:
      return "?";
  }
}

static const char* WebIDLTypeKindName(JS::WebIDLTypeKind kind) {
  switch (kind) {
    case JS::WebIDLTypeKind::Invalid:
      return "invalid";
    case JS::WebIDLTypeKind::Any:
      return "any";
    case JS::WebIDLTypeKind::Undefined:
      return "undefined";
    case JS::WebIDLTypeKind::Boolean:
      return "boolean";
    case JS::WebIDLTypeKind::Byte:
      return "byte";
    case JS::WebIDLTypeKind::Octet:
      return "octet";
    case JS::WebIDLTypeKind::Short:
      return "short";
    case JS::WebIDLTypeKind::UnsignedShort:
      return "unsigned short";
    case JS::WebIDLTypeKind::Long:
      return "long";
    case JS::WebIDLTypeKind::UnsignedLong:
      return "unsigned long";
    case JS::WebIDLTypeKind::LongLong:
      return "long long";
    case JS::WebIDLTypeKind::UnsignedLongLong:
      return "unsigned long long";
    case JS::WebIDLTypeKind::Float:
      return "float";
    case JS::WebIDLTypeKind::UnrestrictedFloat:
      return "unrestricted float";
    case JS::WebIDLTypeKind::Double:
      return "double";
    case JS::WebIDLTypeKind::UnrestrictedDouble:
      return "unrestricted double";
    case JS::WebIDLTypeKind::BigInt:
      return "bigint";
    case JS::WebIDLTypeKind::DOMString:
      return "DOMString";
    case JS::WebIDLTypeKind::USVString:
      return "USVString";
    case JS::WebIDLTypeKind::ByteString:
      return "ByteString";
    case JS::WebIDLTypeKind::Object:
      return "object";
    case JS::WebIDLTypeKind::Symbol:
      return "symbol";
    case JS::WebIDLTypeKind::Interface:
      return "interface";
    case JS::WebIDLTypeKind::Callback:
      return "callback";
    case JS::WebIDLTypeKind::Dictionary:
      return "dictionary";
    case JS::WebIDLTypeKind::Enumeration:
      return "enumeration";
    case JS::WebIDLTypeKind::Sequence:
      return "sequence";
    case JS::WebIDLTypeKind::Record:
      return "record";
    case JS::WebIDLTypeKind::Nullable:
      return "nullable";
    case JS::WebIDLTypeKind::Union:
      return "union";
    case JS::WebIDLTypeKind::BufferSource:
      return "buffer source";
    case JS::WebIDLTypeKind::FrozenArray:
      return "frozen array";
    case JS::WebIDLTypeKind::ObservableArray:
      return "observable array";
    case JS::WebIDLTypeKind::Promise:
      return "promise";
    case JS::WebIDLTypeKind::ReadableStream:
      return "readable stream";
    default:
      return "?";
  }
}

bool js::wasm::BuildCallAdapter(JSContext* cx,
                                const ComponentFuncType& componentFuncType,
                                const JS::WebIDLOperationType& webIDLOpType,
                                ComponentStringEncoding instanceStringEncoding,
                                CallAdapter* out) {
  *out = CallAdapter();

  bool variadic =
      !webIDLOpType.params.empty() && webIDLOpType.params.back().variadic;
  size_t declared = webIDLOpType.params.length();
  size_t fixed = variadic ? declared - 1 : declared;
  size_t nc = componentFuncType.paramTypes.length();

  ValTypeVector flatParamTypes;
  if (!FlattenTypes(componentFuncType.paramTypes, &flatParamTypes)) {
    ReportOutOfMemory(cx);
    return false;
  }
  bool paramsSpilled = flatParamTypes.length() > MaxFlatParams;

  uint32_t flatCursor =
      0;  // running accumulator, only meaningful if !paramsSpilled

  for (size_t i = 0; i < fixed; i++) {
    const JS::WebIDLParam& targetParam = webIDLOpType.params[i];
    ParamStep p;
    if (i < nc) {
      BuildContext ctx =
          paramsSpilled
              ? BuildContext{BuildContext::Kind::Mem,
                             ElemOffset(componentFuncType.paramTypes, i)}
              : BuildContext{BuildContext::Kind::Flat, flatCursor};
      p.source = ParamStep::Source::Positional;
      p.componentIndex = i;
      if (!BuildCoerceStep(cx, componentFuncType.paramTypes[i], ctx,
                           targetParam.type, instanceStringEncoding, &p.step)) {
        return false;
      }
      if (p.step.kind == StepKind::Trap) {
        out->isTrap = true;
        JS_LOG(wasmComponents, Info,
               "component call binding trap: positional param %zu (component "
               "%s) does not coerce to WebIDL param %zu (%s)",
               i, ComponentTypeKindName(componentFuncType.paramTypes[i].kind()),
               i, WebIDLTypeKindName(targetParam.type.kind()));
      }
      if (!paramsSpilled) {
        ValTypeVector flat;
        if (!FlattenType(componentFuncType.paramTypes[i], &flat)) {
          ReportOutOfMemory(cx);
          return false;
        }
        flatCursor += flat.length();
      }
    } else {
      // A missing component arg coerces Undefined through the declared type,
      // same as a JS call site omitting the argument; not hoisted to isTrap
      // since that's not knowable without asking CoerceWebIDLValue.
      p.source = ParamStep::Source::Default;
      p.defaultValue =
          targetParam.defaultValue ? targetParam.defaultValue.ptr() : nullptr;
      p.step.targetType = targetParam.type;
    }
    if (!out->paramSteps.append(std::move(p))) {
      ReportOutOfMemory(cx);
      return false;
    }
  }

  if (variadic) {
    const JS::WebIDLType& elemType = webIDLOpType.params.back().type;
    for (size_t j = fixed; j < nc; j++) {
      BuildContext ctx;
      if (paramsSpilled) {
        ctx = {BuildContext::Kind::Mem,
               ElemOffset(componentFuncType.paramTypes, j)};
      } else {
        uint32_t off;
        if (!FlatOffsetInTypes(componentFuncType.paramTypes, j, &off)) {
          ReportOutOfMemory(cx);
          return false;
        }
        ctx = {BuildContext::Kind::Flat, off};
      }
      ParamStep p;
      p.source = ParamStep::Source::VariadicSpread;
      p.componentIndex = j;
      if (!BuildCoerceStep(cx, componentFuncType.paramTypes[j], ctx, elemType,
                           instanceStringEncoding, &p.step)) {
        return false;
      }
      if (p.step.kind == StepKind::Trap) {
        out->isTrap = true;
        JS_LOG(wasmComponents, Info,
               "component call binding trap: variadic-spread param %zu "
               "(component %s) does not coerce to WebIDL variadic element %s",
               j, ComponentTypeKindName(componentFuncType.paramTypes[j].kind()),
               WebIDLTypeKindName(elemType.kind()));
      }
      if (!out->paramSteps.append(std::move(p))) {
        ReportOutOfMemory(cx);
        return false;
      }
    }
  }
  // Non-variadic excess component params (nc > fixed, no variadic tail) are
  // simply dropped, never converted or passed to the callee.

  if (componentFuncType.resultType.isSome()) {
    const ComponentType& resultType = componentFuncType.resultType.ref();
    if (resultType.kind() == ComponentTypeKind::Result) {
      ComponentResultType r = resultType.asResult();
      out->throwing = true;
      if (r.type) {
        out->resultStep.emplace();
        if (webIDLOpType.returnType) {
          if (!BuildResultStep(cx, *webIDLOpType.returnType, *r.type,
                               out->resultStep.ptr())) {
            return false;
          }
        } else {
          // Permissive per Web.md (undefined coerced through the canonical
          // type); not modeled as a CoerceStep yet, falls back to the stepwise
          // path.
          out->resultStep->kind = StepKind::Trap;
        }
      }
      if (r.errorType) {
        out->errorStep.emplace();
        if (webIDLOpType.returnType) {
          if (!BuildResultStep(cx, *webIDLOpType.returnType, *r.errorType,
                               out->errorStep.ptr())) {
            return false;
          }
        } else {
          out->errorStep->kind = StepKind::Trap;
        }
      }
    } else if (webIDLOpType.returnType) {
      out->resultStep.emplace();
      if (!BuildResultStep(cx, *webIDLOpType.returnType, resultType,
                           out->resultStep.ptr())) {
        return false;
      }
    }
    // else: component expects a (non-result) value but the operation
    // declares no return type - permissive per Web.md, not modeled yet.
  }

  return true;
}

bool js::wasm::SelectWebIDLOverload(
    JSContext* cx, const ComponentFuncType& componentFuncType,
    const JS::WebIDLOperationTypeVector& overloads,
    mozilla::Maybe<size_t>* outIndex) {
  *outIndex = mozilla::Nothing();
  size_t nc = componentFuncType.paramTypes.length();

  auto fixedArity = [](const JS::WebIDLOperationType& op) -> size_t {
    bool v = !op.params.empty() && op.params.back().variadic;
    return v ? op.params.length() - 1 : op.params.length();
  };
  auto isVariadic = [](const JS::WebIDLOperationType& op) -> bool {
    return !op.params.empty() && op.params.back().variadic;
  };
  // The fewest fixed params the component call site may supply: up to
  // (but not including) the first optional param, per WebIDL's "optional
  // params are always trailing" rule. A call site may supply anywhere from
  // here up to fixedArity - BuildCallAdapter fills any params past what it
  // supplies from each one's own declared default (or NotPassed if it has
  // none), rather than requiring every optional param to be spelled out in
  // the component signature.
  auto minArity = [&fixedArity](const JS::WebIDLOperationType& op) -> size_t {
    size_t fixed = fixedArity(op);
    size_t i = 0;
    while (i < fixed && !op.params[i].optional) {
      i++;
    }
    return i;
  };

  mozilla::Vector<size_t, 0, SystemAllocPolicy> candidates;
  for (size_t i = 0; i < overloads.length(); i++) {
    size_t fixed = fixedArity(overloads[i]);
    bool compatible = isVariadic(overloads[i])
                          ? nc >= fixed
                          : nc >= minArity(overloads[i]) && nc <= fixed;
    if (compatible && !candidates.append(i)) {
      ReportOutOfMemory(cx);
      return false;
    }
  }
  if (candidates.length() <= 1) {
    if (!candidates.empty()) {
      *outIndex = mozilla::Some(candidates[0]);
    }
    return true;
  }

  size_t maxFixed = 0;
  for (size_t idx : candidates) {
    maxFixed = std::max(maxFixed, fixedArity(overloads[idx]));
  }

  for (size_t argIdx = 0; argIdx < maxFixed && argIdx < nc; argIdx++) {
    mozilla::Maybe<JS::WebIDLOverloadClass> firstClass;
    bool differ = false;
    for (size_t idx : candidates) {
      if (argIdx >= fixedArity(overloads[idx])) {
        continue;
      }
      JS::WebIDLOverloadClass cls =
          JS::WebIDLOverloadKind(overloads[idx].params[argIdx].type);
      if (!firstClass) {
        firstClass = mozilla::Some(cls);
      } else if (*firstClass != cls) {
        differ = true;
      }
    }
    if (!differ) {
      continue;
    }
    JS::WebIDLType canonical;
    if (!CanonicalWebIDLType(cx, componentFuncType.paramTypes[argIdx],
                             &canonical)) {
      return false;
    }
    JS::WebIDLOverloadClass wantClass = JS::WebIDLOverloadKind(canonical);
    for (size_t idx : candidates) {
      if (argIdx < fixedArity(overloads[idx]) &&
          JS::WebIDLOverloadKind(overloads[idx].params[argIdx].type) ==
              wantClass) {
        *outIndex = mozilla::Some(idx);
        return true;
      }
    }
    return true;  // distinguishing index found, but nothing matched
  }

  // No distinguishing index among fixed params: fall back to the first
  // arity-compatible candidate, matching Web.md's argc-first resolution.
  *outIndex = mozilla::Some(candidates[0]);
  return true;
}

// Result-direction counterpart of BuildVariantStep: canonicalTarget is
// already computed by BuildResultStep, so unlike the read direction this
// never needs to resolve target members by name or match Union arms.
static bool BuildVariantResultStep(JSContext* cx,
                                   const ComponentType& componentResultType,
                                   const JS::WebIDLType& canonicalTarget,
                                   CoerceStep* out) {
  ValTypeVector flatResult;
  if (!FlattenType(componentResultType, &flatResult)) {
    return false;
  }
  bool resultSpills = flatResult.length() > MaxFlatResults;
  out->resultSpills = resultSpills;
  if (resultSpills) {
    out->destAccess.mode = AccessLoc::Mode::Mem;
    out->destAccess.offset = 0;
  }

  out->kind = StepKind::Variant;
  out->resultSize = ElemSize(componentResultType);
  out->resultAlign = Alignment(componentResultType);

  const JS::WebIDLDictionaryMemberVector& members =
      canonicalTarget.def()->schema().as<JS::WebIDLDictionaryType>().members;
  const JS::WebIDLEnumerationType& kindEnum =
      members[0].type.def()->schema().as<JS::WebIDLEnumerationType>();
  bool hasValueMember = members.length() > 1;

  uint32_t numCases = NumBuildVariantCases(componentResultType);
  uint32_t discSize = DiscriminantSize(numCases);
  uint32_t payloadOffset =
      AlignTo(discSize, BuildVariantMaxPayloadAlign(componentResultType));

  if (!out->enumRemap.reserve(numCases) ||
      !out->variantCases.reserve(numCases)) {
    ReportOutOfMemory(cx);
    return false;
  }
  for (uint32_t i = 0; i < numCases; i++) {
    mozilla::Span<const char> label =
        BuildVariantCaseLabel(componentResultType, i);
    mozilla::Maybe<uint32_t> targetIndex =
        kindEnum.find(label.data(), label.Length());
    out->enumRemap.infallibleAppend(targetIndex ? *targetIndex
                                                : CoerceStep::kNoEnumMatch);

    VariantCaseStep caseStep;
    mozilla::Maybe<ComponentType> caseType =
        BuildVariantCaseType(componentResultType, i);
    if (caseType) {
      caseStep.payloadStep = mozilla::MakeUnique<CoerceStep>();
      if (!caseStep.payloadStep) {
        ReportOutOfMemory(cx);
        return false;
      }
      if (!hasValueMember) {
        caseStep.payloadStep->kind = StepKind::Trap;
      } else {
        JS::WebIDLType payloadCanonical;
        if (!CanonicalWebIDLType(cx, *caseType, &payloadCanonical)) {
          return false;
        }
        if (!BuildResultStep(cx, payloadCanonical, *caseType,
                             caseStep.payloadStep.get())) {
          return false;
        }
        // A payload only exists in memory when the whole variant spills - see
        // BuildResultStep's Dictionary case for why a non-spilling variant's
        // case can never have a real (non-empty) payload to address.
        if (resultSpills) {
          caseStep.payloadStep->destAccess.mode = AccessLoc::Mode::Mem;
          caseStep.payloadStep->destAccess.offset = payloadOffset;
        }
      }
    }
    if (!out->variantCases.append(std::move(caseStep))) {
      ReportOutOfMemory(cx);
      return false;
    }
  }
  return true;
}

// The reverse (result) direction, scoped for now to scalars/strings/
// interfaces.
bool js::wasm::BuildResultStep(JSContext* cx,
                               const JS::WebIDLType& webIDLReturnType,
                               const ComponentType& componentResultType,
                               CoerceStep* out) {
  *out = CoerceStep();
  JS::WebIDLType canonicalTarget;
  if (!CanonicalWebIDLType(cx, componentResultType, &canonicalTarget)) {
    return false;
  }
  out->targetType = canonicalTarget;

  // A composite result that doesn't fit in a single flat core value is
  // written through a caller-supplied out-param pointer (see
  // FlattenFuncType's Lower comment), not allocated fresh. Marking the root
  // step Mem-addressed here (offset 0, so it resolves to exactly that
  // pointer) lets the same Mem-addressed write path nested fields/elements
  // already use below handle it, with no separate "am I the top level"
  // branch needed downstream. A nested recursive call's own marking here is
  // immediately overwritten by its parent right after the call returns (see
  // e.g. the Option/Sequence/Dictionary cases below), so this only has an
  // effect for the outermost call.
  ValTypeVector flatResult;
  if (!FlattenType(componentResultType, &flatResult)) {
    return false;
  }
  bool resultSpills = flatResult.length() > MaxFlatResults;

  if (componentResultType.kind() == ComponentTypeKind::Char) {
    // Reverse of BuildCoerceStep's char special case: a component char result
    // is a flat i32 codepoint, not a (ptr,len) string, so this takes the Scalar
    // path.
    out->kind = StepKind::Scalar;
    out->op = CoerceOp::CharFromString;
    return true;
  }

  JS::WebIDLTypeKind srcKind = webIDLReturnType.kind();
  JS::WebIDLTypeKind dstKind = canonicalTarget.kind();

  if (dstKind == JS::WebIDLTypeKind::Boolean) {
    out->kind = StepKind::Scalar;
    out->op = CoerceOp::BoolIdentity;
    return true;
  }
  if (JS::WebIDLTypeKindIsInteger(dstKind)) {
    if (!JS::WebIDLTypeKindIsNumeric(srcKind)) {
      out->kind = StepKind::Trap;
      return true;
    }
    uint8_t srcBits, dstBits;
    bool srcSigned, dstSigned;
    if (JS::WebIDLTypeKindIsInteger(srcKind)) {
      WebIDLIntegerWidthAndSign(srcKind, &srcBits, &srcSigned);
    } else {
      srcBits = JS::WebIDLTypeKindIsUnrestrictedFloat(srcKind) ? 64 : 32;
      srcSigned = true;
    }
    WebIDLIntegerWidthAndSign(dstKind, &dstBits, &dstSigned);
    int64_t srcMin, srcMax, dstMin, dstMax;
    IntegerRange(srcBits, srcSigned, &srcMin, &srcMax);
    IntegerRange(dstBits, dstSigned, &dstMin, &dstMax);
    out->kind = StepKind::Scalar;
    out->srcBits = srcBits;
    out->srcSigned = srcSigned;
    out->targetBits = dstBits;
    out->targetSigned = dstSigned;
    out->rangeMin = dstMin;
    out->rangeMax = dstMax;
    if (!JS::WebIDLTypeKindIsInteger(srcKind)) {
      out->op = CoerceOp::IntModular;  // float->int narrowing: no widen case
    } else if (srcMin >= dstMin && srcMax <= dstMax) {
      out->op =
          srcSigned ? CoerceOp::IntWidenSigned : CoerceOp::IntWidenUnsigned;
    } else {
      out->op = CoerceOp::IntModular;
    }
    return true;
  }
  if (JS::WebIDLTypeKindIsFloat(dstKind)) {
    if (!JS::WebIDLTypeKindIsNumeric(srcKind)) {
      out->kind = StepKind::Trap;
      return true;
    }
    out->kind = StepKind::Scalar;
    out->targetBits = (dstKind == JS::WebIDLTypeKind::Double ||
                       dstKind == JS::WebIDLTypeKind::UnrestrictedDouble)
                          ? 64
                          : 32;
    out->restrictFloat = dstKind == JS::WebIDLTypeKind::Float ||
                         dstKind == JS::WebIDLTypeKind::Double;
    if (JS::WebIDLTypeKindIsInteger(srcKind)) {
      uint8_t srcBits;
      bool srcSigned;
      WebIDLIntegerWidthAndSign(srcKind, &srcBits, &srcSigned);
      out->op =
          srcSigned ? CoerceOp::FloatFromSigned : CoerceOp::FloatFromUnsigned;
    } else {
      bool srcIs64 = JS::WebIDLTypeKindIsUnrestrictedFloat(srcKind) ||
                     srcKind == JS::WebIDLTypeKind::Double;
      bool dstIs64 = out->targetBits == 64;
      out->op = (!srcIs64 && dstIs64)   ? CoerceOp::F32ToF64
                : (srcIs64 && !dstIs64) ? CoerceOp::F64ToF32
                                        : CoerceOp::FloatIdentity;
    }
    return true;
  }
  if (JS::WebIDLTypeKindIsString(dstKind)) {
    out->kind = StepKind::String;
    out->targetString = StringKindFromWebIDLKind(dstKind);
    if (resultSpills) {
      out->destAccess.mode = AccessLoc::Mode::Mem;
      out->destAccess.offset = 0;
    }
    return true;
  }
  if (dstKind == JS::WebIDLTypeKind::Interface ||
      dstKind == JS::WebIDLTypeKind::Object) {
    // componentResultType is Own/Borrow here; mirrors BuildCoerceStep's
    // Interface case, which needs the same three fields to resolve a handle via
    // the handle table. A resource with no WebIDL interface canonicalizes to
    // the generic Object type instead of Interface (see CanonicalWebIDLType's
    // Own/Borrow case), but still needs the same handle-table round-trip;
    // WriteCoercedInterface already falls back to a plain Wrapped handle
    // when expectedInterface is null.
    bool hasInterface = dstKind == JS::WebIDLTypeKind::Interface;
    out->kind = StepKind::Interface;
    out->expectedInterface =
        hasInterface ? canonicalTarget.interface() : nullptr;
    bool resultOwn = componentResultType.kind() == ComponentTypeKind::Own;
    ComponentType resourceType = resultOwn ? componentResultType.asOwn()
                                           : componentResultType.asBorrow();
    out->resourceOwn = resultOwn;
    out->resourceTypeKind = resourceType.kind();
    out->resourceTypeDef = resourceType.typeDef();
    return true;
  }
  if (dstKind == JS::WebIDLTypeKind::Nullable) {
    // Only a simple (non-nested) option<T> canonicalizes to Nullable; a nested
    // option<option<T>> canonicalizes to Dictionary instead.
    if (componentResultType.kind() != ComponentTypeKind::Option) {
      out->kind = StepKind::Trap;
      return true;
    }
    ComponentType inner = componentResultType.asOption();
    out->kind = StepKind::Option;
    out->resultSize = ElemSize(componentResultType);
    out->resultAlign = Alignment(componentResultType);
    out->resultSpills = resultSpills;
    if (resultSpills) {
      out->destAccess.mode = AccessLoc::Mode::Mem;
      out->destAccess.offset = 0;
    }
    out->child = mozilla::MakeUnique<CoerceStep>();
    if (!out->child) {
      ReportOutOfMemory(cx);
      return false;
    }
    if (!BuildResultStep(cx, canonicalTarget.innerType(), inner,
                         out->child.get())) {
      return false;
    }
    // A payload only exists in memory when the option itself spills - a
    // non-spilling option's inner type must itself flatten to zero slots (see
    // BuildResultStep's Dictionary case), so there is nothing to address.
    if (resultSpills) {
      out->child->destAccess.mode = AccessLoc::Mode::Mem;
      out->child->destAccess.offset =
          AlignTo(DiscriminantSize(2), Alignment(inner));
    }
    return true;
  }
  if (dstKind == JS::WebIDLTypeKind::Sequence) {
    // A component tuple also canonicalizes to Sequence but has no single
    // per-element type/stride like list; not modeled yet, falls through to
    // Trap.
    if (componentResultType.kind() != ComponentTypeKind::List) {
      out->kind = StepKind::Trap;
      return true;
    }
    ComponentType elemType = componentResultType.asList();
    out->kind = StepKind::Sequence;
    out->destAccess.elemStride = ElemSize(elemType);
    out->destAccess.elemAlign = Alignment(elemType);
    if (resultSpills) {
      out->destAccess.mode = AccessLoc::Mode::Mem;
      out->destAccess.offset = 0;
    }
    out->child = mozilla::MakeUnique<CoerceStep>();
    if (!out->child) {
      ReportOutOfMemory(cx);
      return false;
    }
    // Recurses against the canonical element type, not webIDLReturnType's own
    // structure - a declared return type more specific than any is only checked
    // for top-level class compatibility by the caller.
    if (!BuildResultStep(cx, canonicalTarget.innerType(), elemType,
                         out->child.get())) {
      return false;
    }
    out->child->destAccess.mode = AccessLoc::Mode::Mem;
    return true;
  }
  if (dstKind == JS::WebIDLTypeKind::Dictionary) {
    // Variant/Result/nested-Option canonicalize to a Dictionary ({kind,
    // value}); only a genuine Record result reaches the rest of this branch.
    if (componentResultType.kind() == ComponentTypeKind::Variant ||
        componentResultType.kind() == ComponentTypeKind::Result ||
        componentResultType.kind() == ComponentTypeKind::Option) {
      return BuildVariantResultStep(cx, componentResultType, canonicalTarget,
                                    out);
    }
    if (componentResultType.kind() != ComponentTypeKind::Record) {
      out->kind = StepKind::Trap;
      return true;
    }
    const ComponentRecordFieldVector& fields = componentResultType.asRecord();
    const JS::WebIDLDictionaryMemberVector& members =
        canonicalTarget.def()->schema().as<JS::WebIDLDictionaryType>().members;
    // members[i] always corresponds to fields[i], since CanonicalWebIDLType's
    // Record case builds dictionary members in field order.
    MOZ_ASSERT(members.length() == fields.length());
    out->kind = StepKind::Dictionary;
    out->resultSize = ElemSize(componentResultType);
    out->resultAlign = Alignment(componentResultType);
    out->resultSpills = resultSpills;
    if (resultSpills) {
      out->destAccess.mode = AccessLoc::Mode::Mem;
      out->destAccess.offset = 0;
    }
    if (!out->dictMembers.reserve(fields.length())) {
      ReportOutOfMemory(cx);
      return false;
    }
    for (size_t i = 0; i < fields.length(); i++) {
      DictMemberStep dm;
      dm.presence = DictMemberStep::Presence::FromSource;
      dm.step = mozilla::MakeUnique<CoerceStep>();
      if (!dm.step) {
        ReportOutOfMemory(cx);
        return false;
      }
      if (!BuildResultStep(cx, members[i].type, fields[i].type,
                           dm.step.get())) {
        return false;
      }
      // A field only lives in memory when the record itself spills; a
      // non-spilling record's field keeps whatever (FlatSlot) addressing
      // BuildResultStep gave it, so its own write appends directly to the
      // flat core result(s) instead.
      if (resultSpills) {
        dm.step->destAccess.mode = AccessLoc::Mode::Mem;
        dm.step->destAccess.offset = RecordFieldOffset(fields, i);
      }
      if (!out->dictMembers.append(std::move(dm))) {
        ReportOutOfMemory(cx);
        return false;
      }
    }
    return true;
  }
  out->kind = StepKind::Trap;
  return true;
}

namespace {
struct CoerceWalkContext {
  JSContext* cx;
  WebIDLContext& abi;
  // Never bounds-checked here: step.access.flatSlot is computed once at
  // adapter-build time (AccessFromContext) against this same param list's
  // flattening, so it is always in range by construction.
  const JS::WasmScalarVal* coreParams;
  // The core type of each entry in coreParams, index-aligned. Only populated
  // by the top-level interpreter walk (BuildJSArgsFromAdapter) and only
  // needed by the Any/Object escape hatch (WalkGenericStep), which is the
  // sole place that must reinterpret the whole flat params array generically
  // rather than through a step that already knows its own leaf's type.
  mozilla::Span<const ValType> coreParamTypes;
};
}  // namespace

static bool ReportComponentTrap(JSContext* cx, const char* what) {
  JS_LOG(wasmComponents, Info, "component call trap: %s", what);
  JS_ReportErrorASCII(cx, "component call trap: %s", what);
  return false;
}

static int64_t SignExtendRaw(uint64_t raw, uint8_t bits, bool isSigned) {
  if (!isSigned || bits >= 64) {
    return int64_t(raw);
  }
  uint64_t mask = (uint64_t(1) << bits) - 1;
  uint64_t signBit = uint64_t(1) << (bits - 1);
  return (raw & signBit) ? int64_t(raw | ~mask) : int64_t(raw);
}

// `types` gives the core wasm type of each entry in `scalars` (index-aligned)
// - a WasmScalarVal never records its own type, so the caller must know it
// statically, from the same flattening that produced `scalars` in the first
// place.
static bool AppendScalarValsAsVals(
    mozilla::Span<const JS::WasmScalarVal> scalars,
    mozilla::Span<const ValType> types, RootedValVector* out) {
  MOZ_ASSERT(scalars.Length() == types.Length());
  for (size_t i = 0; i < scalars.Length(); i++) {
    const JS::WasmScalarVal& sv = scalars[i];
    Val v;
    switch (types[i].kind()) {
      case ValType::Kind::I32:
        v = Val(sv.i32());
        break;
      case ValType::Kind::I64:
        v = Val(sv.i64());
        break;
      case ValType::Kind::F32:
        v = Val(sv.f32());
        break;
      case ValType::Kind::F64:
        v = Val(sv.f64());
        break;
      default:
        MOZ_CRASH(
            "non-scalar core value crossing the WebIDL native call boundary");
    }
    if (!out->append(v)) {
      return false;
    }
  }
  return true;
}

// Reads a scalar CoerceStep's raw value from either the flat param or linear
// memory, matching the canonical ABI's flat-vs-memory addressing.
static bool ReadRawInt(CoerceWalkContext& wc, const CoerceStep& step,
                       uint32_t memBase, uint8_t bits, bool isSigned,
                       int64_t* out) {
  if (step.access.mode == AccessLoc::Mode::FlatSlot) {
    const JS::WasmScalarVal& v = wc.coreParams[step.access.flatSlot];
    uint64_t raw = bits > 32 ? v.i64() : uint64_t(v.i32());
    *out = SignExtendRaw(raw, bits, isSigned);
    return true;
  }
  uint64_t raw;
  if (!ReadUint(wc.abi, memBase + step.access.offset, bits / 8, &raw)) {
    return false;
  }
  *out = SignExtendRaw(raw, bits, isSigned);
  return true;
}

static bool ReadRawFloat(CoerceWalkContext& wc, const CoerceStep& step,
                         uint32_t memBase, uint8_t bits, double* out) {
  if (step.access.mode == AccessLoc::Mode::FlatSlot) {
    const JS::WasmScalarVal& v = wc.coreParams[step.access.flatSlot];
    *out = bits == 64 ? double(v.f64()) : double(v.f32());
    return true;
  }
  uint64_t raw;
  if (!ReadUint(wc.abi, memBase + step.access.offset, bits / 8, &raw)) {
    return false;
  }
  *out = bits == 64 ? double(mozilla::BitwiseCast<double>(raw))
                    : double(mozilla::BitwiseCast<float>(uint32_t(raw)));
  return true;
}

static bool ReadRawU32(CoerceWalkContext& wc, const CoerceStep& step,
                       uint32_t memBase, uint32_t* out) {
  if (step.access.mode == AccessLoc::Mode::FlatSlot) {
    *out = wc.coreParams[step.access.flatSlot].i32();
    return true;
  }
  return ReadUint(wc.abi, memBase + step.access.offset, out);
}

static bool ReadRawPtrLen(CoerceWalkContext& wc, const CoerceStep& step,
                          uint32_t memBase, uint32_t* outPtr,
                          uint32_t* outLen) {
  if (step.access.mode == AccessLoc::Mode::FlatSlot) {
    *outPtr = wc.coreParams[step.access.flatSlot].i32();
    *outLen = wc.coreParams[step.access.flatSlot + 1].i32();
    return true;
  }
  return ReadUint(wc.abi, memBase + step.access.offset, outPtr) &&
         ReadUint(wc.abi, memBase + step.access.offset + 4, outLen);
}

// Applies a Scalar CoerceStep's int CoerceOp to raw, returning a native
// int64_t bit pattern (never boxed to JS::Value) - preserves full 64-bit
// precision for a LongLong/UnsignedLongLong target.
static bool ApplyIntOpRaw(JSContext* cx, const CoerceStep& step, int64_t raw,
                          int64_t* out) {
  int64_t v = raw;
  switch (step.op) {
    case CoerceOp::IntWidenSigned:
    case CoerceOp::IntWidenUnsigned:
    case CoerceOp::IntIdentity:
      break;
    case CoerceOp::IntEnforceRange:
      if (v < step.rangeMin || v > step.rangeMax) {
        return ReportComponentTrap(cx,
                                   "integer out of range for [EnforceRange]");
      }
      break;
    case CoerceOp::IntClamp:
      v = std::min(std::max(v, step.rangeMin), step.rangeMax);
      break;
    case CoerceOp::IntModular:
      if (step.targetBits < 64) {
        uint64_t mask = (uint64_t(1) << step.targetBits) - 1;
        uint64_t masked = uint64_t(v) & mask;
        if (step.targetSigned &&
            (masked & (uint64_t(1) << (step.targetBits - 1)))) {
          masked |= ~mask;
        }
        v = int64_t(masked);
      }
      break;
    default:
      MOZ_CRASH("not an int CoerceOp");
  }
  *out = v;
  return true;
}

// Applies a Scalar CoerceStep's float CoerceOp to raw, returning a native
// double (never boxed to JS::Value).
static bool ApplyFloatOpRaw(JSContext* cx, const CoerceStep& step, double raw,
                            double* out) {
  double v = raw;
  if (step.targetBits == 32) {
    v = double(float(v));
  }
  if (step.restrictFloat && !std::isfinite(v)) {
    return ReportComponentTrap(
        cx, "non-finite value for a restricted float target");
  }
  *out = v;
  return true;
}

static bool ApplyBigIntOp(JSContext* cx, const CoerceStep& step, int64_t raw,
                          JS::MutableHandleValue out) {
  JS::BigInt* bi = step.srcSigned
                       ? JS::BigInt::createFromInt64(cx, raw)
                       : JS::BigInt::createFromUint64(cx, uint64_t(raw));
  if (!bi) {
    return false;
  }
  out.setBigInt(bi);
  return true;
}

static bool ReadMemString(CoerceWalkContext& wc, uint32_t ptr, uint32_t len,
                          JS::MutableHandleValue out) {
  mozilla::Vector<char, 0, SystemAllocPolicy> buf;
  if (!buf.resize(len)) {
    ReportOutOfMemory(wc.cx);
    return false;
  }
  if (len > 0 && !ReadBytes(wc.abi, ptr, buf.begin(), len)) {
    return false;
  }
  JSString* str = JS_NewStringCopyUTF8N(wc.cx, JS::UTF8Chars(buf.begin(), len));
  if (!str) {
    return false;
  }
  out.setString(str);
  return true;
}

static bool WalkCoerceStepToJS(CoerceWalkContext& wc, const CoerceStep& step,
                               uint32_t memBase, JS::MutableHandleValue out);

static bool WalkInterfaceStep(CoerceWalkContext& wc, const CoerceStep& step,
                              uint32_t memBase, JS::MutableHandleValue out) {
  JSObject* object;
  if (!JS::ReadCoercedInterface(wc.abi, wc.coreParams, step, memBase,
                                &object)) {
    return false;
  }
  out.setObject(*object);
  return true;
}

// Creates a fresh byte-typed buffer source holding a copy of the `len` bytes
// at `ptr` in linear memory. Copies rather than aliasing: the resulting object
// outlives this call, while linear memory can later be grown, moved or
// detached.
static bool NewByteBufferSourceFromMemory(JSContext* cx, WebIDLContext& abi,
                                          JS::WebIDLBufferSourceKind kind,
                                          uint32_t ptr, uint32_t len,
                                          JS::MutableHandleObject out) {
  MOZ_ASSERT(JS::WebIDLBufferSourceKindIsBytes(kind));
  RootedObject obj(cx);
  switch (kind) {
    case JS::WebIDLBufferSourceKind::ArrayBuffer:
      obj = JS::NewArrayBuffer(cx, len);
      break;
    case JS::WebIDLBufferSourceKind::Uint8Array:
      obj = JS_NewUint8Array(cx, len);
      break;
    case JS::WebIDLBufferSourceKind::Uint8ClampedArray:
      obj = JS_NewUint8ClampedArray(cx, len);
      break;
    default:
      MOZ_CRASH("not a byte-typed buffer source kind");
  }
  if (!obj) {
    return false;
  }
  if (len > 0) {
    // Fetched only after the allocation above, which can GC and move linear
    // memory.
    uint8_t* base;
    size_t memLen;
    if (!abi.memoryBytes(&base, &memLen)) {
      return false;
    }
    if (uint64_t(ptr) + len > memLen) {
      return ReportComponentTrap(cx, "list<u8> out of bounds of memory");
    }
    JS::AutoCheckCannotGC nogc;
    bool isShared;
    mozilla::Span<uint8_t> dest;
    switch (kind) {
      case JS::WebIDLBufferSourceKind::ArrayBuffer:
        dest = JS::ArrayBuffer::fromObject(obj).getData(&isShared, nogc);
        break;
      case JS::WebIDLBufferSourceKind::Uint8Array:
        dest = JS::Uint8Array::fromObject(obj).getData(&isShared, nogc);
        break;
      case JS::WebIDLBufferSourceKind::Uint8ClampedArray:
        dest = JS::Uint8ClampedArray::fromObject(obj).getData(&isShared, nogc);
        break;
      default:
        MOZ_CRASH("not a byte-typed buffer source kind");
    }
    MOZ_RELEASE_ASSERT(dest.Length() == len);
    memcpy(dest.Elements(), base + ptr, len);
  }
  out.set(obj);
  return true;
}

static bool WalkBufferSourceStep(CoerceWalkContext& wc, const CoerceStep& step,
                                 uint32_t memBase, JS::MutableHandleValue out) {
  uint32_t ptr, len;
  if (!JS::ReadCoercedByteListRange(wc.abi, wc.coreParams, step, memBase, &ptr,
                                    &len)) {
    return false;
  }
  RootedObject obj(wc.cx);
  if (!NewByteBufferSourceFromMemory(wc.cx, wc.abi, step.bufferKind, ptr, len,
                                     &obj)) {
    return false;
  }
  out.setObject(*obj);
  return true;
}

static bool WalkSequenceStep(CoerceWalkContext& wc, const CoerceStep& step,
                             uint32_t memBase, JS::MutableHandleValue out) {
  uint32_t ptr, len;
  if (!JS::ReadCoercedListHeader(wc.abi, wc.coreParams, step, memBase, &ptr,
                                 &len)) {
    return false;
  }
  JS::RootedValueVector elems(wc.cx);
  if (!elems.reserve(len)) {
    ReportOutOfMemory(wc.cx);
    return false;
  }
  RootedValue elemVal(wc.cx);
  for (uint32_t i = 0; i < len; i++) {
    uint32_t elemBase = ptr + i * step.access.elemStride;
    if (!WalkCoerceStepToJS(wc, *step.child, elemBase, &elemVal)) {
      return false;
    }
    elems.infallibleAppend(elemVal);
  }
  JSObject* array = JS::NewArrayObject(wc.cx, elems);
  if (!array) {
    return false;
  }
  out.setObject(*array);
  return true;
}

static bool WalkDictionaryStep(CoerceWalkContext& wc, const CoerceStep& step,
                               uint32_t memBase, JS::MutableHandleValue out) {
  RootedObject obj(wc.cx, JS_NewPlainObject(wc.cx));
  if (!obj) {
    return false;
  }
  RootedValue memberVal(wc.cx);
  for (size_t i = 0; i < step.dictMembers.length(); i++) {
    const DictMemberStep& dm = step.dictMembers[i];
    const JS::WebIDLDictionaryMember& member =
        step.targetType.def()
            ->schema()
            .as<JS::WebIDLDictionaryType>()
            .members[i];
    switch (dm.presence) {
      case DictMemberStep::Presence::FromSource:
        if (!WalkCoerceStepToJS(wc, *dm.step, memBase, &memberVal)) {
          return false;
        }
        break;
      case DictMemberStep::Presence::Default:
        memberVal.setUndefined();
        break;
      case DictMemberStep::Presence::MissingRequired:
        MOZ_CRASH("MissingRequired collapses the whole Dictionary to Trap");
    }
    if (!JS_DefineProperty(wc.cx, obj, member.name.get(), memberVal,
                           JSPROP_ENUMERATE)) {
      return false;
    }
  }
  out.setObject(*obj);
  return true;
}

static bool WalkVariantStep(CoerceWalkContext& wc, const CoerceStep& step,
                            uint32_t memBase, JS::MutableHandleValue out) {
  uint32_t sourceCaseIndex, targetKindIndex;
  if (!JS::ReadCoercedVariantCase(wc.abi, wc.coreParams, step, memBase,
                                  &sourceCaseIndex, &targetKindIndex)) {
    return false;
  }
  RootedObject obj(wc.cx, JS_NewPlainObject(wc.cx));
  if (!obj) {
    return false;
  }
  const JS::WebIDLDictionaryMemberVector& members =
      step.targetType.def()->schema().as<JS::WebIDLDictionaryType>().members;
  const JS::WebIDLDictionaryMember* kindMember = nullptr;
  for (const JS::WebIDLDictionaryMember& m : members) {
    if (strcmp(m.name.get(), "kind") == 0) {
      kindMember = &m;
      break;
    }
  }
  MOZ_ASSERT(kindMember);
  const JS::WebIDLEnumerationType& kindEnum =
      kindMember->type.def()->schema().as<JS::WebIDLEnumerationType>();
  JSString* kindStr =
      JS_NewStringCopyZ(wc.cx, kindEnum.labels[targetKindIndex].get());
  if (!kindStr) {
    return false;
  }
  RootedValue kindVal(wc.cx);
  kindVal.setString(kindStr);
  if (!JS_DefineProperty(wc.cx, obj, "kind", kindVal, JSPROP_ENUMERATE)) {
    return false;
  }
  const VariantCaseStep& caseStep = step.variantCases[sourceCaseIndex];
  if (caseStep.payloadStep) {
    RootedValue valueVal(wc.cx);
    if (!WalkCoerceStepToJS(wc, *caseStep.payloadStep, memBase, &valueVal)) {
      return false;
    }
    if (!JS_DefineProperty(wc.cx, obj, "value", valueVal, JSPROP_ENUMERATE)) {
      return false;
    }
  }
  out.setObject(*obj);
  return true;
}

static bool WalkEnumerationStep(CoerceWalkContext& wc, const CoerceStep& step,
                                uint32_t memBase, JS::MutableHandleValue out) {
  uint32_t targetIndex;
  if (!JS::ReadCoercedEnumIndex(wc.abi, wc.coreParams, step, memBase,
                                &targetIndex)) {
    return false;
  }
  const JS::WebIDLEnumerationType& en =
      step.targetType.def()->schema().as<JS::WebIDLEnumerationType>();
  JSString* str = JS_NewStringCopyZ(wc.cx, en.labels[targetIndex].get());
  if (!str) {
    return false;
  }
  out.setString(str);
  return true;
}

// Any/Object escape hatch: falls back to a generic lift of
// genericSourceType followed by the oracle's ToCanonicalWebIDLValue +
// WebIDLValueToJS path.
static bool WalkGenericStep(CoerceWalkContext& wc, const CoerceStep& step,
                            uint32_t memBase, JS::MutableHandleValue out) {
  ComponentType genericSourceType = ComponentType::fromKindAndTypeDef(
      step.genericSourceTypeKind, step.genericSourceTypeDef);
  ComponentValue cv;
  if (step.access.mode == AccessLoc::Mode::FlatSlot) {
    // The Any/Object escape hatch is the only leaf still routed through the
    // general canonical-ABI lift, so it alone needs coreParams as wasm::Val.
    RootedValVector flat(wc.cx);
    mozilla::Span<const JS::WasmScalarVal> allParams(
        wc.coreParams, wc.coreParamTypes.Length());
    if (!AppendScalarValsAsVals(allParams, wc.coreParamTypes, &flat)) {
      ReportOutOfMemory(wc.cx);
      return false;
    }
    size_t index = step.access.flatSlot;
    if (!LiftFlat(wc.abi, flat, &index, genericSourceType, &cv)) {
      return false;
    }
  } else {
    if (!Load(wc.abi, memBase + step.access.offset, genericSourceType, &cv)) {
      return false;
    }
  }
  JS::RootedWebIDLValue wv(wc.cx);
  if (!ToCanonicalWebIDLValue(wc.cx, cv, &wv, wc.abi.handles())) {
    return false;
  }
  return JS::WebIDLValueToJS(wc.cx, wv, out);
}

static bool WalkCoerceStepToJS(CoerceWalkContext& wc, const CoerceStep& step,
                               uint32_t memBase, JS::MutableHandleValue out) {
  switch (step.kind) {
    case StepKind::Trap:
      return ReportComponentTrap(wc.cx, "value-driven doomed coercion");

    case StepKind::Scalar: {
      switch (step.op) {
        case CoerceOp::BoolIdentity: {
          bool v;
          if (!JS::ReadCoercedBool(wc.abi, wc.coreParams, step, memBase, &v)) {
            return false;
          }
          out.setBoolean(v);
          return true;
        }
        case CoerceOp::CharToString: {
          uint32_t cp;
          if (!JS::ReadCoercedCodePoint(wc.abi, wc.coreParams, step, memBase,
                                        &cp)) {
            return false;
          }
          JSString* str = CodePointToJSString(wc.cx, cp);
          if (!str) {
            return false;
          }
          out.setString(str);
          return true;
        }
        case CoerceOp::BigIntFromSigned:
        case CoerceOp::BigIntFromUnsigned: {
          int64_t raw;
          if (!ReadRawInt(wc, step, memBase, step.srcBits, step.srcSigned,
                          &raw)) {
            return false;
          }
          return ApplyBigIntOp(wc.cx, step, raw, out);
        }
        case CoerceOp::FloatFromSigned:
        case CoerceOp::FloatFromUnsigned:
        case CoerceOp::F32ToF64:
        case CoerceOp::F64ToF32:
        case CoerceOp::FloatIdentity: {
          double v;
          if (!JS::ReadCoercedDouble(wc.abi, wc.coreParams, step, memBase,
                                     &v)) {
            return false;
          }
          out.setNumber(v);
          return true;
        }
        default: {
          int64_t v;
          if (!JS::ReadCoercedInt64(wc.abi, wc.coreParams, step, memBase, &v)) {
            return false;
          }
          if (step.targetBits <= 32) {
            if (step.targetSigned) {
              out.setInt32(int32_t(v));
            } else {
              out.setNumber(double(uint32_t(v)));
            }
          } else if (step.targetSigned) {
            out.setNumber(double(v));
          } else {
            out.setNumber(double(uint64_t(v)));
          }
          return true;
        }
      }
    }

    case StepKind::String: {
      uint32_t ptr, len;
      if (!JS::ReadCoercedStringRange(wc.abi, wc.coreParams, step, memBase,
                                      &ptr, &len)) {
        return false;
      }
      return ReadMemString(wc, ptr, len, out);
    }

    case StepKind::Enumeration:
      return WalkEnumerationStep(wc, step, memBase, out);

    case StepKind::Interface:
      return WalkInterfaceStep(wc, step, memBase, out);

    case StepKind::Nullable:
      // child is never null here: an Option-kind source builds its own
      // StepKind::Option below instead.
      return WalkCoerceStepToJS(wc, *step.child, memBase, out);

    case StepKind::Option: {
      bool hasValue;
      if (!JS::ReadCoercedOptionDiscriminant(wc.abi, wc.coreParams, step,
                                             memBase, &hasValue)) {
        return false;
      }
      if (!hasValue) {
        out.setNull();
        return true;
      }
      return WalkCoerceStepToJS(wc, *step.child, memBase, out);
    }

    case StepKind::Variant:
      return WalkVariantStep(wc, step, memBase, out);

    case StepKind::Sequence:
      return WalkSequenceStep(wc, step, memBase, out);

    case StepKind::Dictionary:
      return WalkDictionaryStep(wc, step, memBase, out);

    case StepKind::Union:
      return WalkCoerceStepToJS(wc, *step.child, memBase, out);

    case StepKind::Any:
    case StepKind::Object:
      return WalkGenericStep(wc, step, memBase, out);

    case StepKind::BufferSource:
      return WalkBufferSourceStep(wc, step, memBase, out);

    default:
      MOZ_CRASH("unhandled StepKind in WalkCoerceStepToJS");
  }
}

static bool BuildJSArgsFromAdapter(JSContext* cx,
                                   const ComponentFuncType& funcType,
                                   const CallAdapter& adapter,
                                   WebIDLContext& abi,
                                   const JS::WasmScalarVal* coreParams,
                                   JS::RootedValueVector& outArgs) {
  MOZ_ASSERT(!adapter.isTrap);
  ValTypeVector flatParamTypes;
  if (!FlattenTypes(funcType.paramTypes, &flatParamTypes)) {
    ReportOutOfMemory(cx);
    return false;
  }
  CoerceWalkContext wc{cx, abi, coreParams, flatParamTypes};

  // Never bounds-checked against an actual param count: like every other
  // WebIDLNativeCall entry, `coreParams` is trusted to hold exactly as many
  // slots as this adapter's build-time flattening expects.
  bool paramsSpilled = flatParamTypes.length() > MaxFlatParams;
  uint32_t spilledBase = paramsSpilled ? coreParams[0].i32() : 0;

  RootedValue argVal(cx);
  for (const ParamStep& p : adapter.paramSteps) {
    switch (p.source) {
      case ParamStep::Source::Positional:
      case ParamStep::Source::VariadicSpread: {
        uint32_t base = paramsSpilled ? spilledBase : 0;
        if (!WalkCoerceStepToJS(wc, p.step, base, &argVal)) {
          return false;
        }
        break;
      }
      case ParamStep::Source::Default: {
        // A missing param coerces Undefined (or its default) through the
        // declared type, same as an ordinary JS call omitting the argument -
        // WebIDL bindings treat "not passed" as "passed undefined".
        JS::RootedWebIDLValue src(cx);
        if (p.defaultValue) {
          src = *p.defaultValue;
        } else {
          src = JS::WebIDLValue::makeUndefined(
              JS::WebIDLType::primitive(JS::WebIDLTypeKind::Undefined));
        }
        JS::RootedWebIDLValue coerced(cx);
        if (!JS::CoerceWebIDLValue(cx, src, p.step.targetType, &coerced)) {
          return false;
        }
        if (!JS::WebIDLValueToJS(cx, coerced, &argVal)) {
          return false;
        }
        break;
      }
    }
    if (!outArgs.append(argVal)) {
      ReportOutOfMemory(cx);
      return false;
    }
  }
  return true;
}

// The result direction's fused walk: coerces a JS return value to raw core
// wasm value(s) via `step`, skipping the ComponentValue/WebIDLValue
// intermediates.
//
// Uses `step.targetType` rather than `declaredType` for conversion: an `any`
// declared type carries no type information, and BuildResultStep may choose
// e.g. BoolIdentity purely from the component result's type, independent of
// declaredType's kind.
static bool WriteResultValueAtStep(JSContext* cx, WebIDLContext& abi,
                                   const CoerceStep& step, uint32_t memBase,
                                   JS::HandleWebIDLValue value,
                                   JS::WasmScalarVal* result) {
  if (step.kind == StepKind::Trap) {
    // Only reachable via a per-case doomed payloadStep (see
    // BuildVariantResultStep) - a value-driven trap, not hoisted.
    return ReportComponentTrap(cx, "value-driven doomed coercion");
  }
  if (step.kind == StepKind::Option) {
    bool hasValue = !value.get().isNull();
    uint32_t ptr;
    if (!JS::WriteCoercedOptionDiscriminant(abi, step, memBase, hasValue,
                                            result, &ptr)) {
      return false;
    }
    if (hasValue &&
        !WriteResultValueAtStep(cx, abi, *step.child, ptr, value, result)) {
      return false;
    }
    return true;
  }
  if (step.kind == StepKind::Variant) {
    const JS::WebIDLValueVector& members =
        value.get().data()->asDictionary().members;
    uint32_t targetKindIndex = members[0].asEnumIndex();
    uint32_t sourceCaseIndex = CoerceStep::kNoEnumMatch;
    for (uint32_t i = 0; i < step.enumRemap.length(); i++) {
      if (step.enumRemap[i] == targetKindIndex) {
        sourceCaseIndex = i;
        break;
      }
    }
    if (sourceCaseIndex == CoerceStep::kNoEnumMatch) {
      return ReportComponentTrap(
          cx, "variant \"kind\" has no counterpart in the component type");
    }
    uint32_t ptr;
    if (!JS::WriteCoercedVariantCase(abi, step, memBase, sourceCaseIndex,
                                     result, &ptr)) {
      return false;
    }
    const VariantCaseStep& caseStep = step.variantCases[sourceCaseIndex];
    if (caseStep.payloadStep) {
      JS::RootedWebIDLValue payloadVal(cx, members[1]);
      if (!WriteResultValueAtStep(cx, abi, *caseStep.payloadStep, ptr,
                                  payloadVal, result)) {
        return false;
      }
    }
    return true;
  }
  if (step.kind == StepKind::String) {
    JS::RootedString str(cx, value.get().asString());
    JS::UniqueChars utf8 = JS_EncodeStringToUTF8(cx, str);
    if (!utf8) {
      return false;
    }
    return JS::WriteCoercedString(abi, step, memBase, utf8.get(),
                                  std::strlen(utf8.get()), result);
  }
  if (step.kind == StepKind::Interface) {
    return JS::WriteCoercedInterface(abi, step, memBase, value.get().asObject(),
                                     result);
  }
  if (step.kind == StepKind::Sequence) {
    // A `list<u8>` component result canonicalizes to sequence<octet>, so the
    // step is a Sequence either way - but when the operation's declared return
    // type is a byte-typed buffer source, the value is a typed array, not a
    // sequence, and the whole thing lowers as one bulk copy.
    if (value.get().type().kind() == JS::WebIDLTypeKind::BufferSource) {
      return JS::WriteCoercedByteListFromBufferSource(
          abi, step, memBase, value.get().asObject(), result);
    }
    const JS::WebIDLValueVector& elements =
        value.get().data()->asSequence().elements;
    uint32_t elemPtr;
    if (!JS::WriteCoercedListHeader(abi, step, memBase,
                                    uint32_t(elements.length()), result,
                                    &elemPtr)) {
      return false;
    }
    JS::RootedWebIDLValue elem(cx);
    for (size_t i = 0; i < elements.length(); i++) {
      elem = elements[i];
      uint32_t elemBase = elemPtr + uint32_t(i) * step.destAccess.elemStride;
      if (!WriteResultValueAtStep(cx, abi, *step.child, elemBase, elem,
                                  result)) {
        return false;
      }
    }
    return true;
  }
  if (step.kind == StepKind::Dictionary) {
    // Three cases: nested under a parent that already gave us a memory
    // address (Mem); the un-addressed root of a spilling result, which owns
    // a fresh buffer; or the un-addressed root of a *non*-spilling result,
    // which owns no buffer at all - every member independently flattens to
    // zero-or-one flat slot(s) of its own, written directly below, with no
    // memory access in between (see WebIDLCoerceStep::resultSpills's doc
    // comment).
    bool nested = step.destAccess.mode == AccessLoc::Mode::Mem;
    uint32_t ptr = 0;
    if (nested) {
      ptr = memBase + step.destAccess.offset;
    } else if (step.resultSpills) {
      if (!abi.callRealloc(0, 0, step.resultAlign, step.resultSize, &ptr)) {
        return false;
      }
    }
    const JS::WebIDLValueVector& members =
        value.get().data()->asDictionary().members;
    MOZ_ASSERT(members.length() == step.dictMembers.length());
    JS::RootedWebIDLValue member(cx);
    for (size_t i = 0; i < step.dictMembers.length(); i++) {
      member = members[i];
      if (!WriteResultValueAtStep(cx, abi, *step.dictMembers[i].step, ptr,
                                  member, result)) {
        return false;
      }
    }
    if (!nested && step.resultSpills) {
      *result = JS::WasmScalarVal::fromI32(ptr);
    }
    return true;
  }

  MOZ_ASSERT(step.kind == StepKind::Scalar);
  bool srcIsFloat = JS::WebIDLTypeKindIsFloat(value.get().type().kind());
  switch (step.op) {
    case CoerceOp::BoolIdentity:
      return JS::WriteCoercedBool(abi, step, memBase, value.get().asBool(),
                                  result);
    case CoerceOp::FloatFromSigned:
    case CoerceOp::FloatFromUnsigned:
    case CoerceOp::F32ToF64:
    case CoerceOp::F64ToF32:
    case CoerceOp::FloatIdentity: {
      double v = srcIsFloat       ? value.get().asDouble()
                 : step.srcSigned ? double(value.get().asSigned())
                                  : double(value.get().asUnsigned());
      return JS::WriteCoercedDouble(abi, step, memBase, v, result);
    }
    case CoerceOp::CharFromString: {
      uint32_t cp;
      if (!FirstCodePoint(cx, value.get().asString(), &cp)) {
        return false;
      }
      return JS::WriteCoercedCodePoint(abi, step, memBase, cp, result);
    }
    case CoerceOp::CharToString:
    case CoerceOp::BigIntFromSigned:
    case CoerceOp::BigIntFromUnsigned:
      MOZ_CRASH(
          "not a result-direction CoerceOp (BuildResultStep never emits it)");
    default: {
      // Remaining ops are integer-target; a float-declared source narrows via
      // IntModular, truncating toward zero first, matching WebIDL's
      // float-to-integer conversion.
      int64_t raw = srcIsFloat       ? int64_t(value.get().asDouble())
                    : step.srcSigned ? value.get().asSigned()
                                     : int64_t(value.get().asUnsigned());
      return JS::WriteCoercedInt64(abi, step, memBase, raw, result);
    }
  }
}

// Fused so far: Scalar, String, Interface (also covers a no-WebIDL-interface
// resource, which canonicalizes to Object but is built as an Interface step -
// see BuildResultStep), Sequence (list only), Dictionary, Option, and
// Variant. Falls back to the stepwise path for Trap/tuple.
static bool WalkResultStepFromJS(JSContext* cx, WebIDLContext& abi,
                                 const CoerceStep& step,
                                 const JS::WebIDLType& declaredType,
                                 mozilla::Span<const ValType> flatResultTypes,
                                 HandleValue jsResult,
                                 RootedValVector& coreResults,
                                 uint32_t resultOutPtr = 0) {
  const JS::WebIDLType& conversionType =
      declaredType.kind() == JS::WebIDLTypeKind::Any ? step.targetType
                                                     : declaredType;
  JS::RootedWebIDLValue webidlResult(cx);
  if (!JS::JSToWebIDLValue(cx, jsResult, conversionType, &webidlResult)) {
    return false;
  }
  JS::WasmScalarVal slot;
  if (!WriteResultValueAtStep(cx, abi, step, resultOutPtr, webidlResult,
                              &slot)) {
    return false;
  }
  // A spilling result (flatResultTypes exceeding MaxFlatResults) is written
  // entirely through resultOutPtr above, leaving `slot` untouched and the
  // wasm-level call returning no flat value at all; otherwise `slot` holds
  // exactly the (0 or 1, per MaxFlatResults) values flatResultTypes expects.
  bool resultSpills = flatResultTypes.Length() > MaxFlatResults;
  mozilla::Span<const JS::WasmScalarVal> writtenSlots =
      resultSpills ? mozilla::Span<const JS::WasmScalarVal>()
                   : mozilla::Span<const JS::WasmScalarVal>(
                         &slot, flatResultTypes.Length());
  mozilla::Span<const ValType> writtenTypes =
      resultSpills ? mozilla::Span<const ValType>() : flatResultTypes;
  return AppendScalarValsAsVals(writtenSlots, writtenTypes, &coreResults);
}

bool JS::ReadCoercedInt32(WebIDLContext& ctx, const WasmScalarVal* coreParams,
                          const WebIDLCoerceStep& step, uint32_t memBase,
                          int32_t* out) {
  MOZ_ASSERT(step.kind == StepKind::Scalar);
  MOZ_ASSERT(step.targetBits <= 32);
  CoerceWalkContext wc{ctx.cx(), ctx, coreParams};
  int64_t raw;
  if (!ReadRawInt(wc, step, memBase, step.srcBits, step.srcSigned, &raw)) {
    return false;
  }
  int64_t v;
  if (!ApplyIntOpRaw(ctx.cx(), step, raw, &v)) {
    return false;
  }
  *out = int32_t(v);
  return true;
}

bool JS::ReadCoercedInt64(WebIDLContext& ctx, const WasmScalarVal* coreParams,
                          const WebIDLCoerceStep& step, uint32_t memBase,
                          int64_t* out) {
  MOZ_ASSERT(step.kind == StepKind::Scalar);
  CoerceWalkContext wc{ctx.cx(), ctx, coreParams};
  int64_t raw;
  if (!ReadRawInt(wc, step, memBase, step.srcBits, step.srcSigned, &raw)) {
    return false;
  }
  return ApplyIntOpRaw(ctx.cx(), step, raw, out);
}

bool JS::ReadCoercedDouble(WebIDLContext& ctx, const WasmScalarVal* coreParams,
                           const WebIDLCoerceStep& step, uint32_t memBase,
                           double* out) {
  MOZ_ASSERT(step.kind == StepKind::Scalar);
  CoerceWalkContext wc{ctx.cx(), ctx, coreParams};
  double raw;
  switch (step.op) {
    case CoerceOp::FloatFromSigned:
    case CoerceOp::FloatFromUnsigned: {
      int64_t rawInt;
      if (!ReadRawInt(wc, step, memBase, step.srcBits, step.srcSigned,
                      &rawInt)) {
        return false;
      }
      raw = step.srcSigned ? double(rawInt) : double(uint64_t(rawInt));
      break;
    }
    case CoerceOp::F32ToF64:
    case CoerceOp::F64ToF32:
    case CoerceOp::FloatIdentity:
      if (!ReadRawFloat(wc, step, memBase, step.srcBits, &raw)) {
        return false;
      }
      break;
    default:
      MOZ_CRASH("not a float CoerceOp");
  }
  return ApplyFloatOpRaw(ctx.cx(), step, raw, out);
}

bool JS::ReadCoercedBool(WebIDLContext& ctx, const WasmScalarVal* coreParams,
                         const WebIDLCoerceStep& step, uint32_t memBase,
                         bool* out) {
  MOZ_ASSERT(step.kind == StepKind::Scalar);
  MOZ_ASSERT(step.op == CoerceOp::BoolIdentity);
  if (step.access.mode == AccessLoc::Mode::FlatSlot) {
    *out = coreParams[step.access.flatSlot].i32() != 0;
    return true;
  }
  uint8_t raw;
  if (!ReadUint(ctx, memBase + step.access.offset, &raw)) {
    return false;
  }
  *out = raw != 0;
  return true;
}

bool JS::ReadCoercedCodePoint(WebIDLContext& ctx,
                              const WasmScalarVal* coreParams,
                              const WebIDLCoerceStep& step, uint32_t memBase,
                              uint32_t* outCp) {
  MOZ_ASSERT(step.kind == StepKind::Scalar);
  MOZ_ASSERT(step.op == CoerceOp::CharToString);
  CoerceWalkContext wc{ctx.cx(), ctx, coreParams};
  return ReadRawU32(wc, step, memBase, outCp);
}

bool JS::ReadCoercedStringRange(WebIDLContext& ctx,
                                const WasmScalarVal* coreParams,
                                const WebIDLCoerceStep& step, uint32_t memBase,
                                uint32_t* outPtr, uint32_t* outLen) {
  MOZ_ASSERT(step.kind == StepKind::String);
  CoerceWalkContext wc{ctx.cx(), ctx, coreParams};
  return ReadRawPtrLen(wc, step, memBase, outPtr, outLen);
}

bool JS::ReadCoercedListHeader(WebIDLContext& ctx,
                               const WasmScalarVal* coreParams,
                               const WebIDLCoerceStep& step, uint32_t memBase,
                               uint32_t* outElemPtr, uint32_t* outLen) {
  MOZ_ASSERT(step.kind == StepKind::Sequence);
  CoerceWalkContext wc{ctx.cx(), ctx, coreParams};
  return ReadRawPtrLen(wc, step, memBase, outElemPtr, outLen);
}

bool JS::ReadCoercedByteListRange(WebIDLContext& ctx,
                                  const WasmScalarVal* coreParams,
                                  const WebIDLCoerceStep& step,
                                  uint32_t memBase, uint32_t* outPtr,
                                  uint32_t* outLen) {
  MOZ_ASSERT(step.kind == StepKind::BufferSource);
  MOZ_ASSERT(step.access.elemStride == 1);
  CoerceWalkContext wc{ctx.cx(), ctx, coreParams};
  if (!ReadRawPtrLen(wc, step, memBase, outPtr, outLen)) {
    return false;
  }
  // The range must be wholly inside linear memory before anyone reads it: a
  // (ptr,len) header comes straight from the guest and is otherwise unchecked.
  uint8_t* base;
  size_t memLen;
  if (!ctx.memoryBytes(&base, &memLen)) {
    return false;
  }
  if (uint64_t(*outPtr) + *outLen > memLen) {
    return ReportComponentTrap(ctx.cx(), "list<u8> out of bounds of memory");
  }
  return true;
}

bool JS::ReadCoercedInterface(WebIDLContext& ctx,
                              const WasmScalarVal* coreParams,
                              const WebIDLCoerceStep& step, uint32_t memBase,
                              JSObject** outObject) {
  MOZ_ASSERT(step.kind == StepKind::Interface);
  CoerceWalkContext wc{ctx.cx(), ctx, coreParams};
  uint32_t index;
  if (!ReadRawU32(wc, step, memBase, &index)) {
    return false;
  }
  ComponentHandleTable* handles = ctx.handles();
  if (!handles) {
    return ReportComponentTrap(ctx.cx(),
                               "an own/borrow handle without a handle table");
  }
  JSObject* object;
  if (step.resourceOwn) {
    ComponentResourceHandle rawHandle;
    if (!handles->remove(ctx.cx(), index, &rawHandle)) {
      return false;
    }
    ComponentResourceHandleOwner handle(rawHandle);
    if (handle->resourceType !=
            ComponentResourceTypeId(step.resourceTypeKind,
                                    step.resourceTypeDef.get()) ||
        !handle->own || handle->numLends != 0) {
      return ReportComponentTrap(ctx.cx(),
                                 "component resource handle mismatch");
    }
    // Uses reflectToJS, not the wrapped object directly: a Native-repr
    // handle has no stored JSObject* and must escape via its interface's
    // reflect op instead.
    object = handle->reflectToJS(ctx.cx());
  } else {
    ComponentResourceHandle handle;
    if (!handles->get(ctx.cx(), index, &handle)) {
      return false;
    }
    if (handle.resourceType !=
        ComponentResourceTypeId(step.resourceTypeKind,
                                step.resourceTypeDef.get())) {
      return ReportComponentTrap(ctx.cx(),
                                 "component resource handle type mismatch");
    }
    object = handle.reflectToJS(ctx.cx());
  }
  if (!object) {
    return ReportComponentTrap(ctx.cx(),
                               "component resource handle has no way to "
                               "reflect to JS");
  }
  *outObject = object;
  return true;
}

bool JS::ReadCoercedInterfaceNative(WebIDLContext& ctx,
                                    const WasmScalarVal* coreParams,
                                    const WebIDLCoerceStep& step,
                                    uint32_t memBase, void** outNative,
                                    WebIDLInterface** outOwnedRef) {
  MOZ_ASSERT(step.kind == StepKind::Interface);
  CoerceWalkContext wc{ctx.cx(), ctx, coreParams};
  uint32_t index;
  if (!ReadRawU32(wc, step, memBase, &index)) {
    return false;
  }
  ComponentHandleTable* handles = ctx.handles();
  void* native;
  WebIDLInterface* ownedRef = nullptr;
  if (step.resourceOwn) {
    ComponentResourceHandle rawHandle;
    if (!handles->remove(ctx.cx(), index, &rawHandle)) {
      return false;
    }
    ComponentResourceHandleOwner handle(rawHandle);
    if (handle->resourceType !=
            ComponentResourceTypeId(step.resourceTypeKind,
                                    step.resourceTypeDef.get()) ||
        !handle->own || handle->numLends != 0) {
      return ReportComponentTrap(ctx.cx(),
                                 "component resource handle mismatch");
    }
    native = handle->resolveNative(step.expectedInterface);
    MOZ_RELEASE_ASSERT(native);
    if (handle->isNative) {
      // Pure transfer: the table's ref on this pointer moves to the caller,
      // released later through the same interface it was added under (see
      // ComponentResourceHandle::release/ComponentHandleTable::drop, which
      // do the same). No AddRef/Release pair needed.
      ownedRef = handle->nativeIface;
      handle.forget();
    } else {
      // Unwrapping a Wrapped object doesn't hand us an existing ref - take
      // one so the pointer survives past this handle table interaction.
      step.expectedInterface->addRefNative(native);
      ownedRef = step.expectedInterface;
    }
  } else {
    // A borrow: peek the entry directly rather than filling a full
    // ComponentResourceHandle snapshot - no ref changes hands either way,
    // the table's own ref (or the lender's, transitively) keeps the pointer
    // alive for the duration of this call.
    const ComponentHandleTable::Entry* entry = handles->peek(index);
    if (!entry) {
      JS_ReportErrorASCII(ctx.cx(), "invalid component resource handle");
      return false;
    }
    if (EntryResourceTypeId(*entry) !=
        ComponentResourceTypeId(step.resourceTypeKind,
                                step.resourceTypeDef.get())) {
      return ReportComponentTrap(ctx.cx(),
                                 "component resource handle type mismatch");
    }
    if (entry->flags & ComponentHandleTable::Native) {
      native = entry->ptr;
    } else {
      native = step.expectedInterface
                   ? step.expectedInterface->unwrapNative(
                         handles->wrappedAt(index))
                   : nullptr;
    }
    MOZ_RELEASE_ASSERT(native);
  }
  *outNative = native;
  *outOwnedRef = ownedRef;
  return true;
}

bool JS::ReadCoercedEnumIndex(WebIDLContext& ctx,
                              const WasmScalarVal* coreParams,
                              const WebIDLCoerceStep& step, uint32_t memBase,
                              uint32_t* outTargetIndex) {
  MOZ_ASSERT(step.kind == StepKind::Enumeration);
  CoerceWalkContext wc{ctx.cx(), ctx, coreParams};
  uint32_t sourceIndex;
  if (!ReadRawU32(wc, step, memBase, &sourceIndex)) {
    return false;
  }
  if (sourceIndex >= step.enumRemap.length()) {
    return ReportComponentTrap(ctx.cx(), "enum label index out of range");
  }
  uint32_t targetIndex = step.enumRemap[sourceIndex];
  if (targetIndex == WebIDLCoerceStep::kNoEnumMatch) {
    return ReportComponentTrap(ctx.cx(),
                               "enum label has no counterpart in the target");
  }
  *outTargetIndex = targetIndex;
  return true;
}

bool JS::ReadCoercedOptionDiscriminant(WebIDLContext& ctx,
                                       const WasmScalarVal* coreParams,
                                       const WebIDLCoerceStep& step,
                                       uint32_t memBase, bool* outHasValue) {
  MOZ_ASSERT(step.kind == StepKind::Option);
  CoerceWalkContext wc{ctx.cx(), ctx, coreParams};
  // The discriminant is exactly DiscriminantSize(2) == 1 byte in memory, not a
  // full i32 - unlike ReadRawU32, which would pull in a following field's
  // bytes.
  int64_t disc;
  if (!ReadRawInt(wc, step, memBase, /* bits = */ 8, /* isSigned = */ false,
                  &disc)) {
    return false;
  }
  *outHasValue = disc != 0;
  return true;
}

bool JS::ReadCoercedVariantCase(WebIDLContext& ctx,
                                const WasmScalarVal* coreParams,
                                const WebIDLCoerceStep& step, uint32_t memBase,
                                uint32_t* outSourceCaseIndex,
                                uint32_t* outTargetKindIndex) {
  MOZ_ASSERT(step.kind == StepKind::Variant);
  CoerceWalkContext wc{ctx.cx(), ctx, coreParams};
  // The discriminant is DiscriminantSize(numCases) bytes in memory, not always
  // a full i32.
  uint32_t discBytes = DiscriminantSize(uint32_t(step.variantCases.length()));
  int64_t raw;
  if (!ReadRawInt(wc, step, memBase, uint8_t(discBytes * 8),
                  /* isSigned = */ false, &raw)) {
    return false;
  }
  uint32_t caseIndex = uint32_t(raw);
  if (caseIndex >= step.variantCases.length()) {
    return ReportComponentTrap(ctx.cx(), "variant case index out of range");
  }
  uint32_t targetIndex = step.enumRemap[caseIndex];
  if (targetIndex == WebIDLCoerceStep::kNoEnumMatch) {
    return ReportComponentTrap(ctx.cx(),
                               "variant case has no counterpart in the target");
  }
  *outSourceCaseIndex = caseIndex;
  *outTargetKindIndex = targetIndex;
  return true;
}

// Shared by every JS::WriteCoerced* below: FlatSlot writes raw's low nbytes
// bytes into *result; Mem writes exactly nbytes at
// memBase + step.destAccess.offset.
static bool WriteRawInt(WebIDLContext& ctx, const CoerceStep& step,
                        uint32_t memBase, uint64_t raw, uint32_t nbytes,
                        JS::WasmScalarVal* result) {
  if (step.destAccess.mode == AccessLoc::Mode::Mem) {
    return WriteUint(ctx, memBase + step.destAccess.offset, raw, nbytes);
  }
  *result = nbytes > 4 ? JS::WasmScalarVal::fromI64(raw)
                       : JS::WasmScalarVal::fromI32(uint32_t(raw));
  return true;
}

static bool WriteRawUInt32(WebIDLContext& ctx, const CoerceStep& step,
                           uint32_t memBase, uint32_t raw,
                           JS::WasmScalarVal* result) {
  if (step.destAccess.mode == AccessLoc::Mode::Mem) {
    return WriteUint(ctx, memBase + step.destAccess.offset, raw);
  }
  *result = JS::WasmScalarVal::fromI32(uint32_t(raw));
  return true;
}

// Writes a String-kind step's (ptr,len) header once the string bytes
// themselves are already in linear memory at `strPtr`: either inline at
// `memBase + step.destAccess.offset`, or - for a flat result - in a freshly
// `realloc`'d 8-byte header whose pointer becomes the flat i32 result.
static bool WriteStringHeader(WebIDLContext& ctx, const CoerceStep& step,
                              uint32_t memBase, uint32_t strPtr, uint32_t len,
                              JS::WasmScalarVal* result) {
  if (step.destAccess.mode == AccessLoc::Mode::Mem) {
    uint32_t offset = memBase + step.destAccess.offset;
    return WriteUint<uint32_t>(ctx, offset, strPtr) &&
           WriteUint<uint32_t>(ctx, offset + 4, len);
  }
  uint32_t headerPtr;
  if (!ctx.callRealloc(0, 0, 4, 8, &headerPtr)) {
    return false;
  }
  if (!WriteUint<uint32_t>(ctx, headerPtr, strPtr) ||
      !WriteUint<uint32_t>(ctx, headerPtr + 4, len)) {
    return false;
  }
  *result = JS::WasmScalarVal::fromI32(headerPtr);
  return true;
}

bool JS::WriteCoercedString(WebIDLContext& ctx, const WebIDLCoerceStep& step,
                            uint32_t memBase, const char* utf8, size_t len,
                            WasmScalarVal* result) {
  MOZ_ASSERT(step.kind == StepKind::String);
  uint32_t strPtr;
  if (!ctx.callRealloc(0, 0, 1, uint32_t(len), &strPtr)) {
    return false;
  }
  if (len > 0 && !WriteBytes(ctx, strPtr, utf8, uint32_t(len))) {
    return false;
  }
  return WriteStringHeader(ctx, step, memBase, strPtr, uint32_t(len), result);
}

bool JS::WriteCoercedStringUTF16(WebIDLContext& ctx,
                                 const WebIDLCoerceStep& step, uint32_t memBase,
                                 mozilla::Span<const char16_t> utf16,
                                 WasmScalarVal* result) {
  MOZ_ASSERT(step.kind == StepKind::String);
  uint32_t codeUnits = uint32_t(utf16.Length());

  uint32_t ptr;
  if (!ctx.callRealloc(0, 0, 1, codeUnits, &ptr)) {
    return false;
  }

  uint8_t* base;
  size_t memLen;
  if (!ctx.memoryBytes(&base, &memLen)) {
    return false;
  }
  if (uint64_t(ptr) + codeUnits > memLen) {
    JS_ReportErrorASCII(ctx.cx(), "canonical ABI: store out of bounds");
    return false;
  }

  uint32_t len;
  if (mozilla::IsAscii(utf16)) {
    auto [read, written] = mozilla::ConvertUtf16toUtf8Partial(
        utf16, mozilla::Span(reinterpret_cast<char*>(base + ptr), codeUnits));
    (void)read;
    MOZ_ASSERT(read == utf16.Length());
    len = uint32_t(written);
  } else {
    uint32_t worstCaseSize = codeUnits * 3;
    if (!ctx.callRealloc(ptr, codeUnits, 1, worstCaseSize, &ptr)) {
      return false;
    }
    if (!ctx.memoryBytes(&base, &memLen)) {
      return false;
    }
    if (uint64_t(ptr) + worstCaseSize > memLen) {
      JS_ReportErrorASCII(ctx.cx(), "canonical ABI: store out of bounds");
      return false;
    }
    len = uint32_t(mozilla::ConvertUtf16toUtf8(
        utf16,
        mozilla::Span(reinterpret_cast<char*>(base + ptr), worstCaseSize)));
    if (worstCaseSize > len &&
        !ctx.callRealloc(ptr, worstCaseSize, 1, len, &ptr)) {
      return false;
    }
  }

  return WriteStringHeader(ctx, step, memBase, ptr, len, result);
}

bool JS::WriteCoercedCodePoint(WebIDLContext& ctx, const WebIDLCoerceStep& step,
                               uint32_t memBase, uint32_t cp,
                               WasmScalarVal* result) {
  MOZ_ASSERT(step.kind == StepKind::Scalar);
  MOZ_ASSERT(step.op == CoerceOp::CharFromString);
  return WriteRawInt(ctx, step, memBase, cp, 4, result);
}

bool JS::WriteCoercedInterface(WebIDLContext& ctx, const WebIDLCoerceStep& step,
                               uint32_t memBase, JSObject* object,
                               WasmScalarVal* result) {
  MOZ_ASSERT(step.kind == StepKind::Interface);
  ComponentHandleTable* handles = ctx.handles();
  if (!handles) {
    return ReportComponentTrap(ctx.cx(),
                               "an own/borrow handle without a handle table");
  }
  bool isSameOrigin = object->compartment() == ctx.cx()->compartment();
  ComponentResourceClassification classification =
      ClassifyResourceObject(object, isSameOrigin, step.expectedInterface);
  uint32_t index;
  if (!handles->add(ctx.cx(),
                    ComponentResourceTypeId(step.resourceTypeKind,
                                            step.resourceTypeDef.get()),
                    step.resourceOwn, classification, &index)) {
    return false;
  }
  return WriteRawInt(ctx, step, memBase, index, 4, result);
}

bool JS::WriteCoercedInterfaceNative(WebIDLContext& ctx,
                                     const WebIDLCoerceStep& step,
                                     uint32_t memBase,
                                     void* alreadyAddRefedNative,
                                     WasmScalarVal* result) {
  MOZ_ASSERT(step.kind == StepKind::Interface);
  MOZ_ASSERT(step.expectedInterface && step.expectedInterface->hasNativeOps());
  ComponentHandleTable* handles = ctx.handles();
  ComponentResourceClassification classification =
      ComponentResourceClassification::AdoptNative(alreadyAddRefedNative,
                                                    step.expectedInterface);
  uint32_t index;
  if (!handles->add(ctx.cx(),
                    ComponentResourceTypeId(step.resourceTypeKind,
                                            step.resourceTypeDef.get()),
                    step.resourceOwn, classification, &index)) {
    step.expectedInterface->releaseNative(alreadyAddRefedNative);
    return false;
  }
  return WriteRawUInt32(ctx, step, memBase, index, result);
}

bool JS::WriteCoercedListHeader(WebIDLContext& ctx,
                                const WebIDLCoerceStep& step, uint32_t memBase,
                                uint32_t count, WasmScalarVal* result,
                                uint32_t* outElemPtr) {
  MOZ_ASSERT(step.kind == StepKind::Sequence);
  uint32_t elemPtr;
  if (!ctx.callRealloc(0, 0, step.destAccess.elemAlign,
                       count * step.destAccess.elemStride, &elemPtr)) {
    return false;
  }
  if (step.destAccess.mode == AccessLoc::Mode::Mem) {
    uint32_t offset = memBase + step.destAccess.offset;
    if (!WriteUint<uint32_t>(ctx, offset, elemPtr) ||
        !WriteUint<uint32_t>(ctx, offset + 4, count)) {
      return false;
    }
  } else {
    uint32_t headerPtr;
    if (!ctx.callRealloc(0, 0, 4, 8, &headerPtr)) {
      return false;
    }
    if (!WriteUint<uint32_t>(ctx, headerPtr, elemPtr) ||
        !WriteUint<uint32_t>(ctx, headerPtr + 4, count)) {
      return false;
    }
    *result = WasmScalarVal::fromI32(headerPtr);
  }
  *outElemPtr = elemPtr;
  return true;
}

bool JS::WriteCoercedByteListFromBufferSource(WebIDLContext& ctx,
                                              const WebIDLCoerceStep& step,
                                              uint32_t memBase,
                                              JSObject* bufferSource,
                                              WasmScalarVal* result) {
  MOZ_ASSERT(step.kind == StepKind::Sequence);
  MOZ_ASSERT(step.destAccess.elemStride == 1);
  JSContext* cx = ctx.cx();

  // Unwrapped once up front (`unwrap` does a CheckedUnwrapStatic), then kept
  // rooted: it has to survive the allocation below, which can GC.
  RootedObject source(
      cx, JS::ArrayBufferOrView::unwrap(bufferSource).asObjectUnbarriered());
  if (!source) {
    return ReportComponentTrap(cx, "list<u8> result is not a buffer source");
  }
  if (JS::ArrayBufferOrView::fromObject(source).isDetached()) {
    return ReportComponentTrap(cx, "list<u8> result is detached");
  }

  bool isBuffer = !!JS::ArrayBuffer::fromObject(source);
  size_t len = isBuffer ? JS::GetArrayBufferByteLength(source)
                        : JS_GetArrayBufferViewByteLength(source);
  if (len > UINT32_MAX) {
    return ReportComponentTrap(cx, "list<u8> result too large");
  }
  uint32_t count = uint32_t(len);

  uint32_t elemPtr;
  // Runs the guest's `realloc`, so this can GC, move linear memory, and even
  // detach or resize `source` - nothing acquired before this point stays valid
  // past it.
  if (!JS::WriteCoercedListHeader(ctx, step, memBase, count, result,
                                  &elemPtr)) {
    return false;
  }
  if (count == 0) {
    return true;
  }

  uint8_t* base;
  size_t memLen;
  if (!ctx.memoryBytes(&base, &memLen)) {
    return false;
  }
  // WriteCoercedListHeader just reallocated `count` bytes at elemPtr, so this
  // is a check on realloc's own behaviour, not a guest-controlled bound.
  MOZ_RELEASE_ASSERT(uint64_t(elemPtr) + count <= memLen);

  if (JS::ArrayBufferOrView::fromObject(source).isDetached()) {
    return ReportComponentTrap(cx, "list<u8> result detached during realloc");
  }
  JS::AutoCheckCannotGC nogc;
  bool isShared;
  mozilla::Span<uint8_t> src =
      isBuffer
          ? JS::ArrayBuffer::fromObject(source).getData(&isShared, nogc)
          : JS::ArrayBufferView::fromObject(source).getData(&isShared, nogc);
  if (src.Length() != count) {
    return ReportComponentTrap(cx,
                               "list<u8> result changed length during realloc");
  }
  memcpy(base + elemPtr, src.Elements(), count);
  return true;
}

// Shared by WriteCoercedOptionDiscriminant/WriteCoercedVariantCase. Three
// cases, mirroring WriteResultValueAtStep's Dictionary case: nested under a
// parent that already gave us a memory address (Mem) - write the
// discriminant there; the un-addressed root of a spilling result - realloc
// a fresh buffer, write the discriminant there, and return its pointer as
// the flat result; or the un-addressed root of a non-spilling result - the
// discriminant itself *is* the (only) flat result value, so it's written
// directly with no memory access at all (see
// WebIDLCoerceStep::resultSpills's doc comment for why a non-spilling
// variant/option can never have a real payload for `*outPtr` to address).
static bool WriteVariantLikeDiscriminant(WebIDLContext& ctx,
                                         const CoerceStep& step,
                                         uint32_t memBase, uint64_t discValue,
                                         uint32_t discBytes,
                                         JS::WasmScalarVal* result,
                                         uint32_t* outPtr) {
  if (step.destAccess.mode == AccessLoc::Mode::Mem) {
    uint32_t ptr = memBase + step.destAccess.offset;
    if (!WriteUint(ctx, ptr, discValue, discBytes)) {
      return false;
    }
    *outPtr = ptr;
    return true;
  }
  if (step.resultSpills) {
    uint32_t ptr;
    if (!ctx.callRealloc(0, 0, step.resultAlign, step.resultSize, &ptr)) {
      return false;
    }
    if (!WriteUint(ctx, ptr, discValue, discBytes)) {
      return false;
    }
    *result = JS::WasmScalarVal::fromI32(ptr);
    *outPtr = ptr;
    return true;
  }
  *result = JS::WasmScalarVal::fromI32(uint32_t(discValue));
  *outPtr = 0;
  return true;
}

bool JS::WriteCoercedOptionDiscriminant(WebIDLContext& ctx,
                                        const WebIDLCoerceStep& step,
                                        uint32_t memBase, bool hasValue,
                                        WasmScalarVal* result,
                                        uint32_t* outPtr) {
  MOZ_ASSERT(step.kind == StepKind::Option);
  return WriteVariantLikeDiscriminant(ctx, step, memBase, hasValue ? 1 : 0,
                                      DiscriminantSize(2), result, outPtr);
}

bool JS::WriteCoercedVariantCase(WebIDLContext& ctx,
                                 const WebIDLCoerceStep& step, uint32_t memBase,
                                 uint32_t caseIndex, WasmScalarVal* result,
                                 uint32_t* outPtr) {
  MOZ_ASSERT(step.kind == StepKind::Variant);
  MOZ_ASSERT(caseIndex < step.variantCases.length());
  uint32_t discBytes = DiscriminantSize(uint32_t(step.variantCases.length()));
  return WriteVariantLikeDiscriminant(ctx, step, memBase, caseIndex, discBytes,
                                      result, outPtr);
}

bool JS::WriteCoercedInt64(WebIDLContext& ctx, const WebIDLCoerceStep& step,
                           uint32_t memBase, int64_t value,
                           WasmScalarVal* result) {
  MOZ_ASSERT(step.kind == StepKind::Scalar);
  int64_t v;
  if (!ApplyIntOpRaw(ctx.cx(), step, value, &v)) {
    return false;
  }
  uint32_t nbytes = step.destAccess.mode == AccessLoc::Mode::Mem
                        ? step.targetBits / 8
                        : (step.targetBits > 32 ? 8 : 4);
  return WriteRawInt(ctx, step, memBase, uint64_t(v), nbytes, result);
}

bool JS::WriteCoercedInt32(WebIDLContext& ctx, const WebIDLCoerceStep& step,
                           uint32_t memBase, int32_t value,
                           WasmScalarVal* result) {
  return WriteCoercedInt64(ctx, step, memBase, int64_t(value), result);
}

bool JS::WriteCoercedDouble(WebIDLContext& ctx, const WebIDLCoerceStep& step,
                            uint32_t memBase, double value,
                            WasmScalarVal* result) {
  MOZ_ASSERT(step.kind == StepKind::Scalar);
  double v;
  if (!ApplyFloatOpRaw(ctx.cx(), step, value, &v)) {
    return false;
  }
  if (step.destAccess.mode == AccessLoc::Mode::Mem) {
    uint32_t offset = memBase + step.destAccess.offset;
    return step.targetBits == 32
               ? WriteUint<uint32_t>(ctx, offset,
                                     mozilla::BitwiseCast<uint32_t>(float(v)))
               : WriteUint<uint64_t>(ctx, offset,
                                     mozilla::BitwiseCast<uint64_t>(v));
  }
  *result = step.targetBits == 32 ? WasmScalarVal::fromF32(float(v))
                                  : WasmScalarVal::fromF64(v);
  return true;
}

bool JS::WriteCoercedBool(WebIDLContext& ctx, const WebIDLCoerceStep& step,
                          uint32_t memBase, bool value, WasmScalarVal* result) {
  MOZ_ASSERT(step.kind == StepKind::Scalar);
  MOZ_ASSERT(step.op == CoerceOp::BoolIdentity);
  return WriteRawInt(ctx, step, memBase, value ? 1 : 0, 1, result);
}

// Statically determines whether `callee` has a native overload matching
// `funcType`, independent of any particular call's actual arguments -
// exactly the selection wasm::LoweredCall used to redo on every call before
// the native-vs-JS decision moved to instantiation time (see
// ComponentInstance::init). Returns false only on OOM/error; on success,
// `*eligible` reports whether a usable overload was found (a trapping,
// "statically doomed" overload is not eligible - the caller should
// synthesize a func that just traps, same as an unimplemented resource
// builtin does). If eligible, `*outEntry`/`*outAdapter` are the selected
// overload's native entry point and compiled call adapter.
[[nodiscard]] static bool SelectNativeOverload(
    JSContext* cx, JS::HandleFunction callee, const ComponentFuncType& funcType,
    bool* eligible, JS::WebIDLNativeCall* outEntry, CallAdapter* outAdapter) {
  *eligible = false;
  if (!callee->hasJitInfo()) {
    JS_LOG(wasmComponents, Info,
           "native call path: not taken (callee has no jitinfo)");
    return true;
  }
  const JSJitInfo* jitInfo = callee->jitInfo();
  if (!jitInfo->isWebIDLTypedJitInfo() ||
      !jitInfo->hasNativeOverloadEntriesJitInfo()) {
    JS_LOG(wasmComponents, Info,
           "native call path: not taken (callee isn't WebIDL-native-typed)");
    return true;
  }
  const auto* methodInfo =
      reinterpret_cast<const JSWebIDLNativeMethodJitInfo*>(jitInfo);

  JS::WebIDLOperationTypeVector overloads;
  if (!methodInfo->base.buildOperationTypes(cx, overloads)) {
    return false;
  }
  mozilla::Maybe<size_t> selected;
  if (!SelectWebIDLOverload(cx, funcType, overloads, &selected)) {
    return false;
  }
  if (!selected || !methodInfo->overloadEntries[*selected]) {
    JS_LOG(wasmComponents, Info, "native call path: not taken (%s)",
           selected ? "overload has no native entry, unsupported by codegen"
                    : "no overload matched the component call site");
    return true;
  }

  if (!BuildCallAdapter(cx, funcType, overloads[*selected],
                        ComponentStringEncoding::Utf8, outAdapter)) {
    return false;
  }
  if (outAdapter->isTrap) {
    JS_LOG(wasmComponents, Info,
           "native call path: not taken (statically doomed binding)");
    return true;
  }
  *eligible = true;
  *outEntry = methodInfo->overloadEntries[*selected];
  JS_LOG(wasmComponents, Info, "native call path: taken (overload %zu)",
         *selected);
  return true;
}

static bool RawCoreValuesToVals(JSContext* cx, JS::HandleValueArray rawArgs,
                                const ComponentTypeVector& types,
                                uint32_t maxFlat, RootedValVector& out) {
  ValTypeVector flatTypes;
  if (!FlattenTypes(types, &flatTypes)) {
    ReportOutOfMemory(cx);
    return false;
  }
  size_t needed = flatTypes.length() > maxFlat ? 1 : flatTypes.length();
  if (rawArgs.length() < needed) {
    JS_ReportErrorASCII(cx, "not enough raw core arguments");
    return false;
  }
  if (flatTypes.length() > maxFlat) {
    RootedVal v(cx);
    if (!Val::fromJSValue(cx, ValType::i32(), rawArgs[0], &v)) {
      return false;
    }
    if (!out.append(v.get())) {
      ReportOutOfMemory(cx);
      return false;
    }
    return true;
  }
  if (!out.reserve(flatTypes.length())) {
    ReportOutOfMemory(cx);
    return false;
  }
  for (size_t i = 0; i < flatTypes.length(); i++) {
    RootedVal v(cx);
    if (!Val::fromJSValue(cx, flatTypes[i], rawArgs[i], &v)) {
      return false;
    }
    out.infallibleAppend(v.get());
  }
  return true;
}

// The inverse: core wasm::Vals -> raw JS values (0 vals -> empty; else one
// JS value per val).
static bool ValsToRawCoreValues(JSContext* cx, HandleValVector vals,
                                JS::RootedValueVector& out) {
  if (!out.reserve(vals.length())) {
    ReportOutOfMemory(cx);
    return false;
  }
  for (size_t i = 0; i < vals.length(); i++) {
    RootedValue v(cx);
    if (!vals[i].get().toJSValue(cx, &v)) {
      return false;
    }
    out.infallibleAppend(v);
  }
  return true;
}

// If type is result<T,E>, populates *okType/*errType (Nothing if that side
// has no payload) and returns true; otherwise returns false.
static bool IsResultType(const ComponentType& type,
                         mozilla::Maybe<ComponentType>* okType,
                         mozilla::Maybe<ComponentType>* errType) {
  if (type.kind() != ComponentTypeKind::Result) {
    return false;
  }
  ComponentResultType r = type.asResult();
  *okType = r.type;
  *errType = r.errorType;
  return true;
}

static bool DefaultWebIDLOperationType(JSContext* cx, bool hasResult,
                                       JS::WebIDLOperationType* out) {
  JS::WebIDLParam param;
  param.type = JS::WebIDLType::primitive(JS::WebIDLTypeKind::Any);
  param.variadic = true;
  if (!out->params.append(std::move(param))) {
    ReportOutOfMemory(cx);
    return false;
  }
  if (hasResult) {
    out->returnType =
        mozilla::Some(JS::WebIDLType::primitive(JS::WebIDLTypeKind::Any));
  }
  return true;
}

// Coerces a JS return value to a component value of targetType: JS ->
// WebIDL(op) -> WebIDL(canonical) -> component. An `any` (or missing)
// declared return type converts straight to the canonical type instead,
// mirroring canon.lift's own JS<->WebIDL boundary handling.
static bool LowerJSReturnValue(JSContext* cx, HandleValue jsResult,
                               const JS::WebIDLOperationType& opType,
                               const ComponentType& targetType,
                               ComponentValue* out,
                               ComponentHandleTable* handles) {
  JS::WebIDLType canonicalType;
  if (!CanonicalWebIDLType(cx, targetType, &canonicalType)) {
    return false;
  }
  JS::RootedWebIDLValue canonicalResult(cx);
  if (opType.returnType.isNothing()) {
    JS::RootedWebIDLValue undef(
        cx, JS::WebIDLValue::makeUndefined(
                JS::WebIDLType::primitive(JS::WebIDLTypeKind::Undefined)));
    if (!JS::CoerceWebIDLValue(cx, undef, canonicalType, &canonicalResult)) {
      return false;
    }
  } else if (opType.returnType.ref().kind() == JS::WebIDLTypeKind::Any) {
    if (!JS::JSToWebIDLValue(cx, jsResult, canonicalType, &canonicalResult)) {
      return false;
    }
  } else {
    JS::RootedWebIDLValue webidlResult(cx);
    if (!JS::JSToWebIDLValue(cx, jsResult, opType.returnType.ref(),
                             &webidlResult)) {
      return false;
    }
    if (!JS::CoerceWebIDLValue(cx, webidlResult, canonicalType,
                               &canonicalResult)) {
      return false;
    }
  }
  return FromCanonicalWebIDLValue(cx, canonicalResult, targetType, out,
                                  handles);
}

// Builds a `result<T,E>`-typed component value for the result<T,E> throwing
// convention in LoweredNativeCall below.
static bool MakeResultComponentValue(JSContext* cx,
                                     const ComponentType& resultType, bool isOk,
                                     mozilla::Maybe<ComponentValue>&& payload,
                                     ComponentValue* out) {
  RefPtr<ComponentValueData> data = js_new<ComponentValueData>(
      ComponentValueData::ResultData{isOk, std::move(payload)});
  if (!data) {
    ReportOutOfMemory(cx);
    return false;
  }
  *out = ComponentValue::makeComposite(resultType, std::move(data));
  return true;
}

bool js::wasm::LiftedCall(JSContext* cx, JS::HandleObject coreCallee,
                          const ComponentFuncType& funcType,
                          JS::WebIDLContext& abiCtx, JS::HandleValue postReturn,
                          const JS::CallArgs& args) {
  JS::WebIDLOperationType opType;
  if (!CanonicalWebIDLOperationType(cx, funcType, &opType)) {
    return false;
  }

  ComponentValueVector componentParams;
  if (!componentParams.reserve(funcType.paramTypes.length())) {
    ReportOutOfMemory(cx);
    return false;
  }
  JS::RootedWebIDLValue webidlParam(cx);
  for (size_t i = 0; i < funcType.paramTypes.length(); i++) {
    if (!JS::JSToWebIDLValue(cx, args.get(i), opType.params[i].type,
                             &webidlParam)) {
      return false;
    }
    ComponentValue componentParam;
    if (!FromCanonicalWebIDLValue(cx, webidlParam, funcType.paramTypes[i],
                                  &componentParam, abiCtx.handles())) {
      return false;
    }
    componentParams.infallibleAppend(componentParam);
  }

  RootedValVector coreParams(cx);
  if (!PrepareCanonABIParams(abiCtx, funcType, componentParams, coreParams)) {
    return false;
  }

  JS::RootedValueVector jsParams(cx);
  if (!ValsToRawCoreValues(cx, coreParams, jsParams)) {
    return false;
  }

  RootedValue rval(cx);
  RootedObject thisv(cx, nullptr);
  RootedValue calleeVal(cx, JS::ObjectValue(*coreCallee));
  if (!JS::Call(cx, thisv, calleeVal, jsParams, &rval)) {
    return false;
  }

  ComponentTypeVector resultTypes;
  if (funcType.resultType.isSome() &&
      !resultTypes.append(funcType.resultType.ref())) {
    ReportOutOfMemory(cx);
    return false;
  }

  RootedValVector coreResults(cx);
  if (!RawCoreValuesToVals(cx, JS::HandleValueArray(rval), resultTypes,
                           MaxFlatResults, coreResults)) {
    return false;
  }

  ComponentValueVector componentResults;
  if (!ReceiveCanonABIResults(abiCtx, funcType, coreResults,
                              &componentResults)) {
    return false;
  }

  // A top-level result<T,E> return type uses the throwing convention from
  // Web.md: error(e) throws the raw payload value, ok(v) returns v (or
  // undefined if ok has no payload).
  RootedValue result(cx);
  RootedValue pendingError(cx);
  bool shouldThrow = false;
  if (funcType.resultType.isSome()) {
    MOZ_ASSERT(componentResults.length() == 1);
    mozilla::Maybe<ComponentType> okType, errType;
    if (!IsResultType(funcType.resultType.ref(), &okType, &errType)) {
      JS::RootedWebIDLValue canonicalResult(cx);
      if (!ToCanonicalWebIDLValue(cx, componentResults[0], &canonicalResult,
                                  abiCtx.handles())) {
        return false;
      }
      if (!JS::WebIDLValueToJS(cx, canonicalResult, &result)) {
        return false;
      }
    } else {
      const ComponentValueData::ResultData& resultData =
          componentResults[0].data()->asResult();
      RootedValue payloadValue(cx);
      if (resultData.payload) {
        JS::RootedWebIDLValue canonicalPayload(cx);
        if (!ToCanonicalWebIDLValue(cx, *resultData.payload, &canonicalPayload,
                                    abiCtx.handles()) ||
            !JS::WebIDLValueToJS(cx, canonicalPayload, &payloadValue)) {
          return false;
        }
      }
      if (!resultData.isOk) {
        pendingError = payloadValue;
        shouldThrow = true;
      } else {
        result = payloadValue;
      }
    }
  }

  // Runs even for a result<T,E> error(e) case (post-return happens before the
  // throw), since the callee already allocated for e; only a genuine JS throw
  // skips this, having exited before any of this.
  if (!postReturn.isUndefined()) {
    JS::RootedValueVector rawResults(cx);
    if (!ValsToRawCoreValues(cx, coreResults, rawResults)) {
      return false;
    }
    RootedValue discardedRval(cx);
    if (!JS::Call(cx, thisv, postReturn, rawResults, &discardedRval)) {
      return false;
    }
  }

  if (shouldThrow) {
    JS_SetPendingException(cx, pendingError);
    return false;
  }

  args.rval().set(result);
  return true;
}

// The fast path: builds `jsArgs` via the fused CoerceStep/CallAdapter walk
// (BuildCallAdapter + BuildJSArgsFromAdapter) instead of the oracle's
// stepwise ComponentValue/WebIDLValue pipeline - no intermediate value is
// materialized per argument.
// Invokes a lowered import's callee the way its bracket tag demands. A
// `[constructor]` import resolves to the interface object itself (see
// ReadComponentFuncImport), and a WebIDL constructor throws when merely
// called, so it has to be a construct. `thisv` is unused in that case: a
// construct derives its receiver from new.target, which is the callee.
[[nodiscard]] static bool InvokeLoweredCallee(JSContext* cx,
                                              HandleObject callee,
                                              HandleValue thisv,
                                              const JS::HandleValueArray& args,
                                              bool isConstructor,
                                              MutableHandleValue rval) {
  RootedValue calleeVal(cx, JS::ObjectValue(*callee));
  if (!isConstructor) {
    return JS::Call(cx, thisv, calleeVal, args, rval);
  }
  RootedObject result(cx);
  if (!JS::Construct(cx, calleeVal, args, &result)) {
    return false;
  }
  rval.setObject(*result);
  return true;
}

// The generic JS::WebIDLNativeCall entry: rather than a native overload
// specialized for one callee's declared signature, drives an arbitrary JS
// callee (which may not even be WebIDL-typed at all) through the same
// adapter machinery, one JS::Value at a time. Recovers the closure that was
// called through - and with it the actual callee, name-kind tag and
// component func type a native overload instead gets to bake into
// `entry`/`adapter` ahead of time - via LoweredNativeClosure::fromCtx.
static bool LoweredNativeCall(const JS::WebIDLCallAdapter* adapterPtr,
                              JS::WebIDLContext* ctx,
                              const JS::WasmScalarVal* params,
                              uint32_t resultMemBase,
                              JS::WasmScalarVal* results) {
  LoweredNativeClosure* closure = LoweredNativeClosure::fromCtx(ctx);
  JSContext* cx = ctx->cx();
  const CallAdapter& adapter = *adapterPtr;

  ComponentInstance* instance = closure->instance;
  uint32_t loweredFuncIndex = closure->loweredFuncIndex;
  RootedObject callee(cx, instance->componentFunc(loweredFuncIndex));
  ComponentNameKind nameKind =
      instance->componentFuncNameKind(loweredFuncIndex);
  ComponentType funcTypeVal =
      instance->component()->getTypeForFunc(loweredFuncIndex);
  const ComponentFuncType& funcType = funcTypeVal.asFunc();

  bool bindThisReceiver = nameKind == ComponentNameKind::Method;
  bool isConstructor = nameKind == ComponentNameKind::Constructor;

  if (adapter.isTrap) {
    return ReportComponentTrap(cx, "statically doomed binding");
  }

  // `callee` (an arbitrary component func: an import, or another lift) has
  // no WebIDL type of its own here, so use the "read the component function
  // import" fallback of a variadic `any` operation type.
  JS::WebIDLOperationType opType;
  if (!DefaultWebIDLOperationType(cx, funcType.resultType.isSome(), &opType)) {
    return false;
  }

  JS::RootedValueVector jsArgs(cx);
  if (!BuildJSArgsFromAdapter(cx, funcType, adapter, *ctx, params, jsArgs)) {
    return false;
  }

  // A `[method]` import's first flat param is `borrow<self>`; bind it as
  // `callee`'s `this` (it resolved to the unbound `Ctor.prototype.member`)
  // rather than passing it positionally. `[static]`/`[constructor]` imports
  // are unaffected. A native-eligible `[method]` callee never reaches here:
  // ComponentInstance::init resolves it straight to a native-call stub,
  // which binds `self` as an ordinary leading interface param instead (the
  // callee's own advertised overload already declares it that way).
  if (bindThisReceiver && jsArgs.empty()) {
    return ReportComponentTrap(cx, "method callee missing self receiver");
  }
  RootedValue thisv(cx);
  if (bindThisReceiver) {
    thisv = jsArgs[0];
  }
  JS::HandleValueArray callArgs =
      bindThisReceiver
          ? JS::HandleValueArray::subarray(jsArgs, 1, jsArgs.length() - 1)
          : JS::HandleValueArray(jsArgs);

  // A result that doesn't fit in a single flat core value is passed as a
  // trailing out-param the core caller allocated (see FlattenFuncType's
  // Lower comment); `resultMemBase` is that pointer, computed by the caller
  // from the same per-instantiation fact (closure->resultOutParamIndex) that
  // decides whether it means anything here.
  mozilla::Maybe<uint32_t> resultOutPtr = closure->resultOutParamIndex >= 0
                                              ? mozilla::Some(resultMemBase)
                                              : mozilla::Nothing();

  // A plain (non-throwing) scalar/string/resource/list/record/option/variant
  // return fuses through WalkResultStepFromJS; anything else falls back to the
  // stepwise tail.
  bool resultFusable =
      !adapter.throwing && adapter.resultStep &&
      (adapter.resultStep->kind == StepKind::Scalar ||
       adapter.resultStep->kind == StepKind::String ||
       adapter.resultStep->kind == StepKind::Interface ||
       adapter.resultStep->kind == StepKind::Sequence ||
       adapter.resultStep->kind == StepKind::Dictionary ||
       adapter.resultStep->kind == StepKind::Option ||
       adapter.resultStep->kind == StepKind::Variant) &&
      opType.returnType &&
      (opType.returnType->kind() == JS::WebIDLTypeKind::Any ||
       JS::WebIDLOverloadKind(*opType.returnType) ==
           JS::WebIDLOverloadKind(adapter.resultStep->targetType));
  if (resultFusable) {
    RootedValue jsResult(cx);
    if (!InvokeLoweredCallee(cx, callee, thisv, callArgs, isConstructor,
                             &jsResult)) {
      return false;
    }
    ValTypeVector flatResultTypes;
    if (!FlattenType(funcType.resultType.ref(), &flatResultTypes)) {
      return false;
    }
    RootedValVector coreResults(cx);
    if (!WalkResultStepFromJS(cx, *ctx, *adapter.resultStep, *opType.returnType,
                              flatResultTypes, jsResult, coreResults,
                              resultOutPtr.valueOr(0))) {
      return false;
    }
    if (coreResults.length() > 0) {
      results[0] = ValToScalarVal(coreResults[0].get());
    }
    return true;
  }

  // The stepwise fallback: applies the result<T,E> throwing convention (a
  // plain JS throw becomes error(e); a normal return, ok(v)), then lowers
  // the return value into `results`.
  mozilla::Maybe<ComponentType> resultOkType, resultErrType;
  bool isResultReturn =
      funcType.resultType.isSome() &&
      IsResultType(funcType.resultType.ref(), &resultOkType, &resultErrType);

  RootedValue jsResult(cx);
  ComponentValueVector componentResults;
  if (!InvokeLoweredCallee(cx, callee, thisv, callArgs, isConstructor,
                           &jsResult)) {
    if (!isResultReturn || !JS_IsExceptionPending(cx)) {
      return false;
    }
    RootedValue exn(cx);
    if (!JS_GetPendingException(cx, &exn)) {
      return false;
    }
    JS_ClearPendingException(cx);
    mozilla::Maybe<ComponentValue> payload;
    if (resultErrType) {
      ComponentValue cv;
      if (!LowerJSReturnValue(cx, exn, opType, *resultErrType, &cv,
                              ctx->handles())) {
        return false;
      }
      payload = mozilla::Some(cv);
    }
    ComponentValue result;
    if (!MakeResultComponentValue(cx, funcType.resultType.ref(), false,
                                  std::move(payload), &result)) {
      return false;
    }
    if (!componentResults.append(result)) {
      ReportOutOfMemory(cx);
      return false;
    }
  } else if (isResultReturn) {
    // result<T,E> ok case: lower through the ok type, then wrap as ok(v).
    mozilla::Maybe<ComponentValue> payload;
    if (resultOkType) {
      ComponentValue cv;
      if (!LowerJSReturnValue(cx, jsResult, opType, *resultOkType, &cv,
                              ctx->handles())) {
        return false;
      }
      payload = mozilla::Some(cv);
    }
    ComponentValue result;
    if (!MakeResultComponentValue(cx, funcType.resultType.ref(), true,
                                  std::move(payload), &result)) {
      return false;
    }
    if (!componentResults.append(result)) {
      ReportOutOfMemory(cx);
      return false;
    }
  } else if (funcType.resultType.isSome()) {
    ComponentValue componentResult;
    if (!LowerJSReturnValue(cx, jsResult, opType, funcType.resultType.ref(),
                            &componentResult, ctx->handles())) {
      return false;
    }
    if (!componentResults.append(componentResult)) {
      ReportOutOfMemory(cx);
      return false;
    }
  }

  RootedValVector coreResults(cx);
  if (!PrepareCanonABIResults(*ctx, funcType, componentResults, coreResults,
                              resultOutPtr)) {
    return false;
  }
  if (coreResults.length() > 0) {
    results[0] = ValToScalarVal(coreResults[0].get());
  }
  return true;
}

#endif  // ENABLE_WASM_COMPONENTS
