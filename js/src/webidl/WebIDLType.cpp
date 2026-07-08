/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <cstring>

#include "js/experimental/WebIDL.h"
#include "js/Utility.h"  // js_new

using namespace JS;

bool WebIDLInterface::isOrDerivedFrom(const WebIDLInterface* other) const {
  for (const WebIDLInterface* i = this; i; i = i->parent_.get()) {
    if (i == other) {
      return true;
    }
  }
  return false;
}

mozilla::Maybe<uint32_t> WebIDLEnumerationType::find(const char* label,
                                                     size_t length) const {
  for (uint32_t i = 0; i < labels.length(); i++) {
    const char* candidate = labels[i].get();
    if (std::strlen(candidate) == length &&
        std::memcmp(candidate, label, length) == 0) {
      return mozilla::Some(i);
    }
  }
  return mozilla::Nothing();
}

#define WEBIDL_MAKE_DEF(SCHEMA, KIND, FLAGS)            \
  do {                                                  \
    WebIDLTypeDef* def = js_new<WebIDLTypeDef>(SCHEMA); \
    if (!def) {                                         \
      return false;                                     \
    }                                                   \
    *out = WebIDLType((KIND), (FLAGS), def);            \
    return true;                                        \
  } while (0)

bool WebIDLType::sequence(const WebIDLType& element, WebIDLType* out) {
  WEBIDL_MAKE_DEF(WebIDLTypeSchema(element), WebIDLTypeKind::Sequence,
                  WebIDLTypeFlag_None);
}
bool WebIDLType::frozenArray(const WebIDLType& element, WebIDLType* out) {
  WEBIDL_MAKE_DEF(WebIDLTypeSchema(element), WebIDLTypeKind::FrozenArray,
                  WebIDLTypeFlag_None);
}
bool WebIDLType::observableArray(const WebIDLType& element, WebIDLType* out) {
  WEBIDL_MAKE_DEF(WebIDLTypeSchema(element), WebIDLTypeKind::ObservableArray,
                  WebIDLTypeFlag_None);
}
bool WebIDLType::nullable(const WebIDLType& inner, WebIDLType* out) {
  MOZ_ASSERT(inner.kind() != WebIDLTypeKind::Nullable,
             "nullable of nullable is not a WebIDL type");
  WEBIDL_MAKE_DEF(WebIDLTypeSchema(inner), WebIDLTypeKind::Nullable,
                  WebIDLTypeFlag_None);
}
bool WebIDLType::promise(const WebIDLType& inner, WebIDLType* out) {
  WEBIDL_MAKE_DEF(WebIDLTypeSchema(inner), WebIDLTypeKind::Promise,
                  WebIDLTypeFlag_None);
}
bool WebIDLType::record(WebIDLTypeKind keyStringKind, const WebIDLType& value,
                        WebIDLType* out) {
  MOZ_ASSERT(WebIDLTypeKindIsString(keyStringKind));
  WebIDLRecordType rec{keyStringKind, value};
  WEBIDL_MAKE_DEF(WebIDLTypeSchema(std::move(rec)), WebIDLTypeKind::Record,
                  WebIDLTypeFlag_None);
}
bool WebIDLType::union_(WebIDLTypeVector&& members, WebIDLType* out) {
  WebIDLUnionType un{std::move(members)};
  WEBIDL_MAKE_DEF(WebIDLTypeSchema(std::move(un)), WebIDLTypeKind::Union,
                  WebIDLTypeFlag_None);
}
bool WebIDLType::enumeration(
    mozilla::Vector<JS::UniqueChars, 0, js::SystemAllocPolicy>&& labels,
    WebIDLType* out) {
  WebIDLEnumerationType en{std::move(labels)};
  WEBIDL_MAKE_DEF(WebIDLTypeSchema(std::move(en)), WebIDLTypeKind::Enumeration,
                  WebIDLTypeFlag_None);
}
bool WebIDLType::interface_(RefPtr<WebIDLInterface> iface, WebIDLType* out) {
  WEBIDL_MAKE_DEF(WebIDLTypeSchema(std::move(iface)), WebIDLTypeKind::Interface,
                  WebIDLTypeFlag_None);
}
bool WebIDLType::callback(WebIDLType* out) {
  WEBIDL_MAKE_DEF(WebIDLTypeSchema(WebIDLCallbackType{}),
                  WebIDLTypeKind::Callback, WebIDLTypeFlag_None);
}
bool WebIDLType::bufferSource(WebIDLBufferSourceKind kind, uint8_t flags,
                              WebIDLType* out) {
  WEBIDL_MAKE_DEF(WebIDLTypeSchema(WebIDLBufferSourceType{kind}),
                  WebIDLTypeKind::BufferSource, flags);
}
bool WebIDLType::readableStream(WebIDLType* out) {
  WEBIDL_MAKE_DEF(WebIDLTypeSchema(mozilla::Nothing()),
                  WebIDLTypeKind::ReadableStream, WebIDLTypeFlag_None);
}
bool WebIDLType::dictionary(WebIDLType* out) {
  WEBIDL_MAKE_DEF(WebIDLTypeSchema(WebIDLDictionaryType{}),
                  WebIDLTypeKind::Dictionary, WebIDLTypeFlag_None);
}

#undef WEBIDL_MAKE_DEF

bool WebIDLType::addDictionaryMember(JS::UniqueChars name,
                                     const WebIDLType& type, bool required) {
  MOZ_ASSERT(kind_ == WebIDLTypeKind::Dictionary);
  WebIDLDictionaryType& dict = def_->schema().as<WebIDLDictionaryType>();
  return dict.members.append(
      WebIDLDictionaryMember{std::move(name), type, required});
}

const WebIDLType& WebIDLType::innerType() const {
  MOZ_ASSERT(def_);
  return def_->schema().as<WebIDLType>();
}
WebIDLTypeKind WebIDLType::recordKeyKind() const {
  return def_->schema().as<WebIDLRecordType>().keyStringKind;
}
const WebIDLType& WebIDLType::recordValueType() const {
  return def_->schema().as<WebIDLRecordType>().valueType;
}
WebIDLInterface* WebIDLType::interface() const {
  return def_->schema().as<RefPtr<WebIDLInterface>>().get();
}
WebIDLBufferSourceKind WebIDLType::bufferSourceKind() const {
  return def_->schema().as<WebIDLBufferSourceType>().kind;
}

static bool LabelsEqual(
    const mozilla::Vector<JS::UniqueChars, 0, js::SystemAllocPolicy>& a,
    const mozilla::Vector<JS::UniqueChars, 0, js::SystemAllocPolicy>& b) {
  if (a.length() != b.length()) {
    return false;
  }
  for (size_t i = 0; i < a.length(); i++) {
    if (std::strcmp(a[i].get(), b[i].get()) != 0) {
      return false;
    }
  }
  return true;
}

bool WebIDLType::structurallyEqual(const WebIDLType& a, const WebIDLType& b) {
  if (a.kind_ != b.kind_ || a.flags_ != b.flags_) {
    return false;
  }
  if (WebIDLTypeKindIsPrimitive(a.kind_)) {
    return true;
  }
  switch (a.kind_) {
    case WebIDLTypeKind::Sequence:
    case WebIDLTypeKind::FrozenArray:
    case WebIDLTypeKind::ObservableArray:
    case WebIDLTypeKind::Nullable:
    case WebIDLTypeKind::Promise:
      return structurallyEqual(a.innerType(), b.innerType());
    case WebIDLTypeKind::Record:
      return a.recordKeyKind() == b.recordKeyKind() &&
             structurallyEqual(a.recordValueType(), b.recordValueType());
    case WebIDLTypeKind::Union: {
      const WebIDLTypeVector& am =
          a.def_->schema().as<WebIDLUnionType>().members;
      const WebIDLTypeVector& bm =
          b.def_->schema().as<WebIDLUnionType>().members;
      if (am.length() != bm.length()) {
        return false;
      }
      for (size_t i = 0; i < am.length(); i++) {
        if (!structurallyEqual(am[i], bm[i])) {
          return false;
        }
      }
      return true;
    }
    case WebIDLTypeKind::Dictionary: {
      const WebIDLDictionaryMemberVector& am =
          a.def_->schema().as<WebIDLDictionaryType>().members;
      const WebIDLDictionaryMemberVector& bm =
          b.def_->schema().as<WebIDLDictionaryType>().members;
      if (am.length() != bm.length()) {
        return false;
      }
      for (size_t i = 0; i < am.length(); i++) {
        if (std::strcmp(am[i].name.get(), bm[i].name.get()) != 0 ||
            am[i].required != bm[i].required ||
            !structurallyEqual(am[i].type, bm[i].type)) {
          return false;
        }
      }
      return true;
    }
    case WebIDLTypeKind::Enumeration:
      return LabelsEqual(a.def_->schema().as<WebIDLEnumerationType>().labels,
                         b.def_->schema().as<WebIDLEnumerationType>().labels);
    case WebIDLTypeKind::Interface:
      return a.interface() == b.interface();
    case WebIDLTypeKind::BufferSource:
      return a.bufferSourceKind() == b.bufferSourceKind();
    case WebIDLTypeKind::Callback:
    case WebIDLTypeKind::ReadableStream:
      return true;
    default:
      MOZ_CRASH("unhandled WebIDLTypeKind in structurallyEqual");
  }
}
