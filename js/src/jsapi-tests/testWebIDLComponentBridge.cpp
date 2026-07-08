/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jsapi-tests/tests.h"

#ifdef ENABLE_WASM_COMPONENTS

#  include "mozilla/Maybe.h"

#  include "js/experimental/WebIDL.h"
#  include "js/String.h"   // JS_StringEqualsLiteral
#  include "js/Utility.h"  // js_new, js::DuplicateString
#  include "wasm/WasmComponent.h"

using namespace js;
using namespace js::wasm;
using JS::WebIDLTypeKind;

// Round-trips a component value through ToCanonicalWebIDLValue and
// FromCanonicalWebIDLValue, checking the canonical WebIDL type/value along
// the way.
BEGIN_TEST(testWebIDLBridgeScalars) {
  {
    ComponentType t = ComponentType::primitive(ComponentTypeKind::Bool);
    ComponentValue cv = ComponentValue::makeBool(t, true);
    JS::RootedWebIDLValue wv(cx);
    CHECK(ToCanonicalWebIDLValue(cx, cv, &wv));
    CHECK(wv.get().type().kind() == WebIDLTypeKind::Boolean);
    CHECK(wv.get().asBool() == true);
    ComponentValue back;
    CHECK(FromCanonicalWebIDLValue(cx, wv, t, &back));
    CHECK(back.kind() == ComponentTypeKind::Bool);
    CHECK(back.asBool() == true);
  }

  {
    ComponentType t = ComponentType::primitive(ComponentTypeKind::S32);
    ComponentValue cv = ComponentValue::makeSigned(t, -7);
    JS::RootedWebIDLValue wv(cx);
    CHECK(ToCanonicalWebIDLValue(cx, cv, &wv));
    CHECK(wv.get().type().kind() == WebIDLTypeKind::Long);
    CHECK(wv.get().asSigned() == -7);
    ComponentValue back;
    CHECK(FromCanonicalWebIDLValue(cx, wv, t, &back));
    CHECK(back.asSigned() == -7);
  }

  {
    ComponentType t = ComponentType::primitive(ComponentTypeKind::U32);
    ComponentValue cv = ComponentValue::makeUnsigned(t, 42);
    JS::RootedWebIDLValue wv(cx);
    CHECK(ToCanonicalWebIDLValue(cx, cv, &wv));
    CHECK(wv.get().type().kind() == WebIDLTypeKind::UnsignedLong);
    CHECK(wv.get().asUnsigned() == 42);
    ComponentValue back;
    CHECK(FromCanonicalWebIDLValue(cx, wv, t, &back));
    CHECK(back.asUnsigned() == 42);
  }

  {
    ComponentType t = ComponentType::primitive(ComponentTypeKind::F64);
    ComponentValue cv = ComponentValue::makeF64(t, 3.5);
    JS::RootedWebIDLValue wv(cx);
    CHECK(ToCanonicalWebIDLValue(cx, cv, &wv));
    CHECK(wv.get().type().kind() == WebIDLTypeKind::UnrestrictedDouble);
    CHECK(wv.get().asDouble() == 3.5);
    ComponentValue back;
    CHECK(FromCanonicalWebIDLValue(cx, wv, t, &back));
    CHECK(back.asF64() == 3.5);
  }

  // char -> USVString of length 1
  {
    ComponentType t = ComponentType::primitive(ComponentTypeKind::Char);
    ComponentValue cv = ComponentValue::makeUnsigned(t, uint32_t('A'));
    JS::RootedWebIDLValue wv(cx);
    CHECK(ToCanonicalWebIDLValue(cx, cv, &wv));
    CHECK(wv.get().type().kind() == WebIDLTypeKind::USVString);
    bool eq;
    CHECK(JS_StringEqualsLiteral(cx, wv.get().asString(), "A", &eq));
    CHECK(eq);
    ComponentValue back;
    CHECK(FromCanonicalWebIDLValue(cx, wv, t, &back));
    CHECK(back.asUnsigned() == uint32_t('A'));
  }

  return true;
}
END_TEST(testWebIDLBridgeScalars)

BEGIN_TEST(testWebIDLBridgeString) {
  ComponentType t = ComponentType::primitive(ComponentTypeKind::String);
  ComponentValueData::StringData sd;
  CHECK(sd.utf8.append("hi", 2));
  RefPtr<ComponentValueData> data = js_new<ComponentValueData>(std::move(sd));
  CHECK(data);
  ComponentValue cv = ComponentValue::makeComposite(t, data);

  JS::RootedWebIDLValue wv(cx);
  CHECK(ToCanonicalWebIDLValue(cx, cv, &wv));
  CHECK(wv.get().type().kind() == WebIDLTypeKind::USVString);
  bool eq;
  CHECK(JS_StringEqualsLiteral(cx, wv.get().asString(), "hi", &eq));
  CHECK(eq);

  ComponentValue back;
  CHECK(FromCanonicalWebIDLValue(cx, wv, t, &back));
  CHECK(back.kind() == ComponentTypeKind::String);
  const auto& bytes = back.data()->asString().utf8;
  CHECK(bytes.length() == 2);
  CHECK(bytes[0] == 'h');
  CHECK(bytes[1] == 'i');
  return true;
}
END_TEST(testWebIDLBridgeString)

BEGIN_TEST(testWebIDLBridgeList) {
  ComponentType u32 = ComponentType::primitive(ComponentTypeKind::U32);
  ComponentType listType;
  CHECK(ComponentType::list(ComponentType::primitive(ComponentTypeKind::U32),
                            &listType));

  ComponentValueData::ElementsData ed;
  CHECK(ed.elements.append(ComponentValue::makeUnsigned(u32, 10)));
  CHECK(ed.elements.append(ComponentValue::makeUnsigned(u32, 20)));
  RefPtr<ComponentValueData> data = js_new<ComponentValueData>(std::move(ed));
  CHECK(data);
  ComponentValue cv = ComponentValue::makeComposite(listType, data);

  JS::RootedWebIDLValue wv(cx);
  CHECK(ToCanonicalWebIDLValue(cx, cv, &wv));
  CHECK(wv.get().type().kind() == WebIDLTypeKind::Sequence);
  CHECK(wv.get().data()->asSequence().elements.length() == 2);
  CHECK(wv.get().data()->asSequence().elements[0].asUnsigned() == 10);
  CHECK(wv.get().data()->asSequence().elements[1].asUnsigned() == 20);

  ComponentValue back;
  CHECK(FromCanonicalWebIDLValue(cx, wv, listType, &back));
  CHECK(back.kind() == ComponentTypeKind::List);
  CHECK(back.data()->asElements().elements.length() == 2);
  CHECK(back.data()->asElements().elements[0].asUnsigned() == 10);
  CHECK(back.data()->asElements().elements[1].asUnsigned() == 20);
  return true;
}
END_TEST(testWebIDLBridgeList)

static CacheableName BridgeName(const char* s) {
  return CacheableName::fromUTF8Chars(DuplicateString(s));
}

BEGIN_TEST(testWebIDLBridgeEnum) {
  CacheableNameVector labels;
  CHECK(labels.append(BridgeName("red")));
  CHECK(labels.append(BridgeName("green")));
  CHECK(labels.append(BridgeName("blue")));
  ComponentType enumType;
  CHECK(ComponentType::enum_(std::move(labels), &enumType));

  ComponentValue cv = ComponentValue::makeUnsigned(enumType, 2);  // "blue"
  JS::RootedWebIDLValue wv(cx);
  CHECK(ToCanonicalWebIDLValue(cx, cv, &wv));
  CHECK(wv.get().type().kind() == WebIDLTypeKind::Enumeration);
  CHECK(wv.get().asEnumIndex() == 2);

  ComponentValue back;
  CHECK(FromCanonicalWebIDLValue(cx, wv, enumType, &back));
  CHECK(back.kind() == ComponentTypeKind::Enum);
  CHECK(back.asUnsigned() == 2);
  return true;
}
END_TEST(testWebIDLBridgeEnum)

BEGIN_TEST(testWebIDLBridgeOption) {
  ComponentType u32 = ComponentType::primitive(ComponentTypeKind::U32);
  ComponentType optType;
  CHECK(ComponentType::option(ComponentType::primitive(ComponentTypeKind::U32),
                              &optType));

  // some(5): canonical value is stored with the inner (unsigned long) type.
  {
    ComponentValueData::OptionData od;
    od.payload = mozilla::Some(ComponentValue::makeUnsigned(u32, 5));
    RefPtr<ComponentValueData> data = js_new<ComponentValueData>(std::move(od));
    CHECK(data);
    ComponentValue cv = ComponentValue::makeComposite(optType, data);
    JS::RootedWebIDLValue wv(cx);
    CHECK(ToCanonicalWebIDLValue(cx, cv, &wv));
    CHECK(wv.get().type().kind() == WebIDLTypeKind::UnsignedLong);
    CHECK(wv.get().asUnsigned() == 5);
    ComponentValue back;
    CHECK(FromCanonicalWebIDLValue(cx, wv, optType, &back));
    CHECK(back.kind() == ComponentTypeKind::Option);
    CHECK(back.data()->asOption().payload.isSome());
    CHECK(back.data()->asOption().payload->asUnsigned() == 5);
  }

  // none: canonical value is a null nullable.
  {
    ComponentValueData::OptionData od;
    RefPtr<ComponentValueData> data = js_new<ComponentValueData>(std::move(od));
    CHECK(data);
    ComponentValue cv = ComponentValue::makeComposite(optType, data);
    JS::RootedWebIDLValue wv(cx);
    CHECK(ToCanonicalWebIDLValue(cx, cv, &wv));
    CHECK(wv.get().type().kind() == WebIDLTypeKind::Nullable);
    CHECK(wv.get().isNull());
    ComponentValue back;
    CHECK(FromCanonicalWebIDLValue(cx, wv, optType, &back));
    CHECK(back.kind() == ComponentTypeKind::Option);
    CHECK(back.data()->asOption().payload.isNothing());
  }

  return true;
}
END_TEST(testWebIDLBridgeOption)

#endif  // ENABLE_WASM_COMPONENTS
