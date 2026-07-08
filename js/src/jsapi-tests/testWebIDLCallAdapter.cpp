/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jsapi-tests/tests.h"

#ifdef ENABLE_WASM_COMPONENTS

#  include "mozilla/Maybe.h"

#  include "js/experimental/WebIDL.h"
#  include "js/Utility.h"  // js_new, js::DuplicateString
#  include "wasm/WasmComponent.h"

using namespace js;
using namespace js::wasm;
using JS::WebIDLTypeKind;

// paramNames are placeholders: BuildCallAdapter never reads them, but
// ComponentFuncType::operator== release-asserts they're present.
static ComponentFuncType MakeFuncType(ComponentTypeVector&& params,
                                      mozilla::Maybe<ComponentType> result) {
  CacheableNameVector names;
  for (size_t i = 0; i < params.length(); i++) {
    MOZ_RELEASE_ASSERT(
        names.append(CacheableName::fromUTF8Chars(DuplicateString("p"))));
  }
  return ComponentFuncType{std::move(params), std::move(names), result};
}

static ComponentFuncType MakeFuncType(
    std::initializer_list<ComponentType> params,
    mozilla::Maybe<ComponentType> result) {
  ComponentTypeVector vec;
  for (const ComponentType& t : params) {
    MOZ_RELEASE_ASSERT(vec.append(t));
  }
  return MakeFuncType(std::move(vec), result);
}

static JS::WebIDLOperationType MakeOpType(
    std::initializer_list<JS::WebIDLType> paramTypes,
    mozilla::Maybe<JS::WebIDLType> returnType, bool variadic = false) {
  JS::WebIDLOperationType op;
  size_t i = 0;
  for (const JS::WebIDLType& t : paramTypes) {
    JS::WebIDLParam p;
    p.type = t;
    p.variadic = variadic && (i == paramTypes.size() - 1);
    MOZ_RELEASE_ASSERT(op.params.append(std::move(p)));
    i++;
  }
  op.returnType = returnType;
  return op;
}

BEGIN_TEST(testCallAdapter_scalars) {
  ComponentType s32 = ComponentType::primitive(ComponentTypeKind::S32);
  ComponentFuncType funcType = MakeFuncType({s32, s32}, mozilla::Some(s32));

  JS::WebIDLType longType = JS::WebIDLType::primitive(WebIDLTypeKind::Long);
  JS::WebIDLOperationType opType =
      MakeOpType({longType, longType}, mozilla::Some(longType));

  CallAdapter adapter;
  CHECK(BuildCallAdapter(cx, funcType, opType, ComponentStringEncoding::Utf8,
                         &adapter));
  CHECK(!adapter.isTrap);
  CHECK(adapter.paramSteps.length() == 2);

  CHECK(adapter.paramSteps[0].source == ParamStep::Source::Positional);
  CHECK(adapter.paramSteps[0].componentIndex == 0);
  CHECK(adapter.paramSteps[0].step.kind == StepKind::Scalar);
  CHECK(adapter.paramSteps[0].step.access.mode == AccessLoc::Mode::FlatSlot);
  CHECK(adapter.paramSteps[0].step.access.flatSlot == 0);
  CHECK(adapter.paramSteps[0].step.op == CoerceOp::IntWidenSigned);
  CHECK(adapter.paramSteps[0].step.targetBits == 32);
  CHECK(adapter.paramSteps[0].step.targetSigned);

  CHECK(adapter.paramSteps[1].step.access.flatSlot == 1);

  CHECK(adapter.resultStep.isSome());
  CHECK(adapter.resultStep->kind == StepKind::Scalar);
  CHECK(!adapter.throwing);
  return true;
}
END_TEST(testCallAdapter_scalars)

// EnforceRange/Clamp/modular selection, plus a genuine cross-kind mismatch
// that traps.
BEGIN_TEST(testCallAdapter_intCoercions) {
  ComponentType s32 = ComponentType::primitive(ComponentTypeKind::S32);

  {
    ComponentFuncType funcType = MakeFuncType({s32}, mozilla::Nothing());
    JS::WebIDLType byteType = JS::WebIDLType::primitive(
        WebIDLTypeKind::Byte, JS::WebIDLTypeFlag_EnforceRange);
    JS::WebIDLOperationType opType = MakeOpType({byteType}, mozilla::Nothing());
    CallAdapter adapter;
    CHECK(BuildCallAdapter(cx, funcType, opType, ComponentStringEncoding::Utf8,
                           &adapter));
    CHECK(!adapter.isTrap);
    CHECK(adapter.paramSteps[0].step.op == CoerceOp::IntEnforceRange);
    CHECK(adapter.paramSteps[0].step.rangeMin == -128);
    CHECK(adapter.paramSteps[0].step.rangeMax == 127);
  }

  {
    ComponentFuncType funcType = MakeFuncType({s32}, mozilla::Nothing());
    JS::WebIDLType octetType = JS::WebIDLType::primitive(
        WebIDLTypeKind::Octet, JS::WebIDLTypeFlag_Clamp);
    JS::WebIDLOperationType opType =
        MakeOpType({octetType}, mozilla::Nothing());
    CallAdapter adapter;
    CHECK(BuildCallAdapter(cx, funcType, opType, ComponentStringEncoding::Utf8,
                           &adapter));
    CHECK(!adapter.isTrap);
    CHECK(adapter.paramSteps[0].step.op == CoerceOp::IntClamp);
  }

  {
    ComponentFuncType funcType = MakeFuncType({s32}, mozilla::Nothing());
    JS::WebIDLType octetType = JS::WebIDLType::primitive(WebIDLTypeKind::Octet);
    JS::WebIDLOperationType opType =
        MakeOpType({octetType}, mozilla::Nothing());
    CallAdapter adapter;
    CHECK(BuildCallAdapter(cx, funcType, opType, ComponentStringEncoding::Utf8,
                           &adapter));
    CHECK(!adapter.isTrap);
    CHECK(adapter.paramSteps[0].step.op == CoerceOp::IntModular);
  }

  {
    ComponentFuncType funcType = MakeFuncType({s32}, mozilla::Nothing());
    JS::WebIDLType strType =
        JS::WebIDLType::primitive(WebIDLTypeKind::DOMString);
    JS::WebIDLOperationType opType = MakeOpType({strType}, mozilla::Nothing());
    CallAdapter adapter;
    CHECK(BuildCallAdapter(cx, funcType, opType, ComponentStringEncoding::Utf8,
                           &adapter));
    CHECK(adapter.isTrap);
    CHECK(adapter.paramSteps[0].step.kind == StepKind::Trap);
  }

  return true;
}
END_TEST(testCallAdapter_intCoercions)

// A component `char` targets a string kind via the Scalar/CharToString path,
// not the String path (no (ptr,len) in memory for a bare scalar).
BEGIN_TEST(testCallAdapter_charToString) {
  ComponentType charType = ComponentType::primitive(ComponentTypeKind::Char);
  ComponentFuncType funcType = MakeFuncType({charType}, mozilla::Nothing());
  JS::WebIDLType usvType = JS::WebIDLType::primitive(WebIDLTypeKind::USVString);
  JS::WebIDLOperationType opType = MakeOpType({usvType}, mozilla::Nothing());

  CallAdapter adapter;
  CHECK(BuildCallAdapter(cx, funcType, opType, ComponentStringEncoding::Utf8,
                         &adapter));
  CHECK(!adapter.isTrap);
  CHECK(adapter.paramSteps[0].step.kind == StepKind::Scalar);
  CHECK(adapter.paramSteps[0].step.op == CoerceOp::CharToString);
  return true;
}
END_TEST(testCallAdapter_charToString)

BEGIN_TEST(testCallAdapter_string) {
  ComponentType strType = ComponentType::primitive(ComponentTypeKind::String);
  ComponentFuncType funcType = MakeFuncType({strType}, mozilla::Nothing());
  JS::WebIDLType domStrType =
      JS::WebIDLType::primitive(WebIDLTypeKind::DOMString);
  JS::WebIDLOperationType opType = MakeOpType({domStrType}, mozilla::Nothing());

  CallAdapter adapter;
  CHECK(BuildCallAdapter(cx, funcType, opType, ComponentStringEncoding::Utf8,
                         &adapter));
  CHECK(!adapter.isTrap);
  CHECK(adapter.paramSteps[0].step.kind == StepKind::String);
  CHECK(adapter.paramSteps[0].step.targetString == WebIDLStringKind::DOMString);
  CHECK(adapter.paramSteps[0].step.access.mode == AccessLoc::Mode::FlatSlot);
  CHECK(adapter.paramSteps[0].step.access.flatSlot == 0);  // occupies slots 0,1
  return true;
}
END_TEST(testCallAdapter_string)

// Record -> Dictionary: fields resolve by name; a missing required member
// traps the whole Dictionary step (and hoists to the adapter).
BEGIN_TEST(testCallAdapter_recordToDictionary) {
  ComponentType s32 = ComponentType::primitive(ComponentTypeKind::S32);
  ComponentType strType = ComponentType::primitive(ComponentTypeKind::String);

  ComponentRecordFieldVector fields;
  CHECK(fields.append(ComponentRecordField{
      CacheableName::fromUTF8Chars(DuplicateString("x")), s32}));
  CHECK(fields.append(ComponentRecordField{
      CacheableName::fromUTF8Chars(DuplicateString("y")), strType}));
  ComponentType recordType;
  CHECK(ComponentType::record(std::move(fields), &recordType));

  ComponentFuncType funcType = MakeFuncType({recordType}, mozilla::Nothing());

  JS::WebIDLType longType = JS::WebIDLType::primitive(WebIDLTypeKind::Long);
  JS::WebIDLType strTargetType =
      JS::WebIDLType::primitive(WebIDLTypeKind::DOMString);

  {
    JS::WebIDLType dictType;
    CHECK(JS::WebIDLType::dictionary(&dictType));
    CHECK(dictType.addDictionaryMember(DuplicateString("x"), longType,
                                       /* required */ true));
    CHECK(dictType.addDictionaryMember(DuplicateString("y"), strTargetType,
                                       /* required */ false));

    JS::WebIDLOperationType opType = MakeOpType({dictType}, mozilla::Nothing());
    CallAdapter adapter;
    CHECK(BuildCallAdapter(cx, funcType, opType, ComponentStringEncoding::Utf8,
                           &adapter));
    CHECK(!adapter.isTrap);
    CoerceStep& dictStep = adapter.paramSteps[0].step;
    CHECK(dictStep.kind == StepKind::Dictionary);
    CHECK(dictStep.dictMembers.length() == 2);
    CHECK(dictStep.dictMembers[0].presence ==
          DictMemberStep::Presence::FromSource);
    CHECK(dictStep.dictMembers[0].sourceField == 0);
    CHECK(dictStep.dictMembers[0].step->kind == StepKind::Scalar);
    CHECK(dictStep.dictMembers[1].presence ==
          DictMemberStep::Presence::FromSource);
    CHECK(dictStep.dictMembers[1].sourceField == 1);
    CHECK(dictStep.dictMembers[1].step->kind == StepKind::String);
  }

  {
    JS::WebIDLType dictType;
    CHECK(JS::WebIDLType::dictionary(&dictType));
    CHECK(dictType.addDictionaryMember(DuplicateString("z"), longType,
                                       /* required */ true));

    JS::WebIDLOperationType opType = MakeOpType({dictType}, mozilla::Nothing());
    CallAdapter adapter;
    CHECK(BuildCallAdapter(cx, funcType, opType, ComponentStringEncoding::Utf8,
                           &adapter));
    CHECK(adapter.isTrap);
    CHECK(adapter.paramSteps[0].step.kind == StepKind::Trap);
  }

  return true;
}
END_TEST(testCallAdapter_recordToDictionary)

// list<s32> -> sequence<long>: the list itself is FlatSlot-addressed (fits
// under MaxFlatParams), its element is Mem-addressed relative to the
// (runtime-loaded) list pointer, at offset 0.
BEGIN_TEST(testCallAdapter_sequence) {
  ComponentType s32 = ComponentType::primitive(ComponentTypeKind::S32);
  ComponentType listType;
  CHECK(ComponentType::list(ComponentType::primitive(ComponentTypeKind::S32),
                            &listType));
  ComponentFuncType funcType = MakeFuncType({listType}, mozilla::Nothing());

  JS::WebIDLType longType = JS::WebIDLType::primitive(WebIDLTypeKind::Long);
  JS::WebIDLType seqType;
  CHECK(JS::WebIDLType::sequence(longType, &seqType));
  JS::WebIDLOperationType opType = MakeOpType({seqType}, mozilla::Nothing());

  CallAdapter adapter;
  CHECK(BuildCallAdapter(cx, funcType, opType, ComponentStringEncoding::Utf8,
                         &adapter));
  CHECK(!adapter.isTrap);
  CoerceStep& seqStep = adapter.paramSteps[0].step;
  CHECK(seqStep.kind == StepKind::Sequence);
  CHECK(seqStep.access.mode == AccessLoc::Mode::FlatSlot);
  CHECK(seqStep.access.elemStride == 4);  // ElemSize(s32)
  CHECK(seqStep.child);
  CHECK(seqStep.child->kind == StepKind::Scalar);
  CHECK(seqStep.child->access.mode == AccessLoc::Mode::Mem);
  CHECK(seqStep.child->access.offset == 0);
  (void)s32;
  return true;
}
END_TEST(testCallAdapter_sequence)

// A doomed coercion under a Sequence's element type doesn't hoist to
// CallAdapter::isTrap (an empty list would never reach it), but the same
// doom under a Nullable or Union arm does, since both are unconditionally
// reached whenever the wrapper itself is reached.
BEGIN_TEST(testCallAdapter_reachability) {
  ComponentType listOfStr;
  CHECK(ComponentType::list(ComponentType::primitive(ComponentTypeKind::String),
                            &listOfStr));
  JS::WebIDLType longType = JS::WebIDLType::primitive(WebIDLTypeKind::Long);
  JS::WebIDLType seqOfLong;
  CHECK(JS::WebIDLType::sequence(longType, &seqOfLong));

  {
    ComponentFuncType funcType = MakeFuncType({listOfStr}, mozilla::Nothing());
    JS::WebIDLOperationType opType =
        MakeOpType({seqOfLong}, mozilla::Nothing());
    CallAdapter adapter;
    CHECK(BuildCallAdapter(cx, funcType, opType, ComponentStringEncoding::Utf8,
                           &adapter));
    CHECK(!adapter.isTrap);
    CHECK(adapter.paramSteps[0].step.kind == StepKind::Sequence);
    CHECK(adapter.paramSteps[0].step.child->kind == StepKind::Trap);
  }

  {
    ComponentType strType = ComponentType::primitive(ComponentTypeKind::String);
    ComponentFuncType funcType = MakeFuncType({strType}, mozilla::Nothing());
    JS::WebIDLType nullableLong;
    CHECK(JS::WebIDLType::nullable(longType, &nullableLong));
    JS::WebIDLOperationType opType =
        MakeOpType({nullableLong}, mozilla::Nothing());
    CallAdapter adapter;
    CHECK(BuildCallAdapter(cx, funcType, opType, ComponentStringEncoding::Utf8,
                           &adapter));
    CHECK(adapter.isTrap);
    CHECK(adapter.paramSteps[0].step.kind == StepKind::Trap);
  }

  {
    ComponentType s32 = ComponentType::primitive(ComponentTypeKind::S32);
    ComponentFuncType funcType = MakeFuncType({s32}, mozilla::Nothing());
    JS::WebIDLType boolType =
        JS::WebIDLType::primitive(WebIDLTypeKind::Boolean);
    JS::WebIDLType strType2 =
        JS::WebIDLType::primitive(WebIDLTypeKind::DOMString);
    JS::WebIDLTypeVector arms;
    CHECK(arms.append(boolType));
    CHECK(arms.append(strType2));
    JS::WebIDLType unionType;
    CHECK(JS::WebIDLType::union_(std::move(arms), &unionType));
    JS::WebIDLOperationType opType =
        MakeOpType({unionType}, mozilla::Nothing());
    CallAdapter adapter;
    CHECK(BuildCallAdapter(cx, funcType, opType, ComponentStringEncoding::Utf8,
                           &adapter));
    CHECK(adapter.isTrap);
    CHECK(adapter.paramSteps[0].step.kind == StepKind::Trap);
  }

  return true;
}
END_TEST(testCallAdapter_reachability)

// A param with no corresponding component arg coerces Undefined through the
// declared type, whether optional or required - it doesn't hoist to isTrap
// (e.g. Undefined -> `long` succeeds as 0, it doesn't trap).
BEGIN_TEST(testCallAdapter_missingParams) {
  JS::WebIDLType longType = JS::WebIDLType::primitive(WebIDLTypeKind::Long);

  {
    ComponentFuncType funcType = MakeFuncType({}, mozilla::Nothing());
    JS::WebIDLOperationType opType;
    JS::WebIDLParam p;
    p.type = longType;
    p.optional = true;
    CHECK(opType.params.append(std::move(p)));
    CallAdapter adapter;
    CHECK(BuildCallAdapter(cx, funcType, opType, ComponentStringEncoding::Utf8,
                           &adapter));
    CHECK(!adapter.isTrap);
    CHECK(adapter.paramSteps[0].source == ParamStep::Source::Default);
    CHECK(!adapter.paramSteps[0].defaultValue);
  }

  {
    ComponentFuncType funcType = MakeFuncType({}, mozilla::Nothing());
    JS::WebIDLOperationType opType = MakeOpType({longType}, mozilla::Nothing());
    CallAdapter adapter;
    CHECK(BuildCallAdapter(cx, funcType, opType, ComponentStringEncoding::Utf8,
                           &adapter));
    CHECK(!adapter.isTrap);
    CHECK(adapter.paramSteps[0].source == ParamStep::Source::Default);
    CHECK(!adapter.paramSteps[0].defaultValue);
  }

  return true;
}
END_TEST(testCallAdapter_missingParams)

// A variadic tail: fixed prefix positional, remaining component args spread
// against the variadic element type.
BEGIN_TEST(testCallAdapter_variadic) {
  ComponentType s32 = ComponentType::primitive(ComponentTypeKind::S32);
  ComponentFuncType funcType =
      MakeFuncType({s32, s32, s32}, mozilla::Nothing());

  JS::WebIDLType longType = JS::WebIDLType::primitive(WebIDLTypeKind::Long);
  JS::WebIDLOperationType opType =
      MakeOpType({longType, longType}, mozilla::Nothing(), /* variadic */ true);

  CallAdapter adapter;
  CHECK(BuildCallAdapter(cx, funcType, opType, ComponentStringEncoding::Utf8,
                         &adapter));
  CHECK(!adapter.isTrap);
  // 1 fixed + 2 spread (component arity 3, fixed prefix 1).
  CHECK(adapter.paramSteps.length() == 3);
  CHECK(adapter.paramSteps[0].source == ParamStep::Source::Positional);
  CHECK(adapter.paramSteps[1].source == ParamStep::Source::VariadicSpread);
  CHECK(adapter.paramSteps[1].componentIndex == 1);
  CHECK(adapter.paramSteps[2].source == ParamStep::Source::VariadicSpread);
  CHECK(adapter.paramSteps[2].componentIndex == 2);
  CHECK(adapter.paramSteps[2].step.access.flatSlot == 2);
  return true;
}
END_TEST(testCallAdapter_variadic)

// >MaxFlatParams(16): the params spill to a single pointer, and each param's
// AccessLoc becomes Mem-addressed at its offset within the spilled tuple.
BEGIN_TEST(testCallAdapter_spilledParams) {
  ComponentType s32 = ComponentType::primitive(ComponentTypeKind::S32);
  ComponentTypeVector params;
  JS::WebIDLOperationType opType;
  JS::WebIDLType longType = JS::WebIDLType::primitive(WebIDLTypeKind::Long);
  for (int i = 0; i < 17; i++) {
    CHECK(params.append(s32));
    JS::WebIDLParam p;
    p.type = longType;
    CHECK(opType.params.append(std::move(p)));
  }
  ComponentFuncType funcType =
      MakeFuncType(std::move(params), mozilla::Nothing());

  CallAdapter adapter;
  CHECK(BuildCallAdapter(cx, funcType, opType, ComponentStringEncoding::Utf8,
                         &adapter));
  CHECK(!adapter.isTrap);
  CHECK(adapter.paramSteps.length() == 17);
  for (size_t i = 0; i < 17; i++) {
    CHECK(adapter.paramSteps[i].step.access.mode == AccessLoc::Mode::Mem);
    CHECK(adapter.paramSteps[i].step.access.offset ==
          i * 4);  // s32: align 4, size 4
  }
  return true;
}
END_TEST(testCallAdapter_spilledParams)

// Static overload selection: arity alone distinguishes `foo(long)` from
// `foo(long, long)`.
BEGIN_TEST(testSelectWebIDLOverload_arity) {
  ComponentType s32 = ComponentType::primitive(ComponentTypeKind::S32);
  JS::WebIDLType longType = JS::WebIDLType::primitive(WebIDLTypeKind::Long);

  JS::WebIDLOperationTypeVector overloads;
  CHECK(overloads.append(MakeOpType({longType}, mozilla::Nothing())));
  CHECK(overloads.append(MakeOpType({longType, longType}, mozilla::Nothing())));

  {
    ComponentFuncType funcType = MakeFuncType({s32}, mozilla::Nothing());
    mozilla::Maybe<size_t> selected;
    CHECK(SelectWebIDLOverload(cx, funcType, overloads, &selected));
    CHECK(selected.isSome());
    CHECK(*selected == 0);
  }
  {
    ComponentFuncType funcType = MakeFuncType({s32, s32}, mozilla::Nothing());
    mozilla::Maybe<size_t> selected;
    CHECK(SelectWebIDLOverload(cx, funcType, overloads, &selected));
    CHECK(selected.isSome());
    CHECK(*selected == 1);
  }
  {
    ComponentFuncType funcType =
        MakeFuncType({s32, s32, s32}, mozilla::Nothing());
    mozilla::Maybe<size_t> selected;
    CHECK(SelectWebIDLOverload(cx, funcType, overloads, &selected));
    CHECK(selected.isNothing());
  }
  return true;
}
END_TEST(testSelectWebIDLOverload_arity)

// Overload selection by class, among arity-compatible candidates.
BEGIN_TEST(testSelectWebIDLOverload_class) {
  ComponentType s32 = ComponentType::primitive(ComponentTypeKind::S32);
  ComponentType strType = ComponentType::primitive(ComponentTypeKind::String);
  JS::WebIDLType longType = JS::WebIDLType::primitive(WebIDLTypeKind::Long);
  JS::WebIDLType strTargetType =
      JS::WebIDLType::primitive(WebIDLTypeKind::DOMString);

  JS::WebIDLOperationTypeVector overloads;
  CHECK(overloads.append(MakeOpType({longType}, mozilla::Nothing())));
  CHECK(overloads.append(MakeOpType({strTargetType}, mozilla::Nothing())));

  {
    ComponentFuncType funcType = MakeFuncType({s32}, mozilla::Nothing());
    mozilla::Maybe<size_t> selected;
    CHECK(SelectWebIDLOverload(cx, funcType, overloads, &selected));
    CHECK(selected.isSome());
    CHECK(*selected == 0);
  }
  {
    ComponentFuncType funcType = MakeFuncType({strType}, mozilla::Nothing());
    mozilla::Maybe<size_t> selected;
    CHECK(SelectWebIDLOverload(cx, funcType, overloads, &selected));
    CHECK(selected.isSome());
    CHECK(*selected == 1);
  }
  return true;
}
END_TEST(testSelectWebIDLOverload_class)

// BuildResultStep: the reverse direction, scoped to scalars/strings/
// interfaces for this first cut.
BEGIN_TEST(testBuildResultStep_scalar) {
  ComponentType s32 = ComponentType::primitive(ComponentTypeKind::S32);
  JS::WebIDLType longType = JS::WebIDLType::primitive(WebIDLTypeKind::Long);

  CoerceStep step;
  CHECK(BuildResultStep(cx, longType, s32, &step));
  CHECK(step.kind == StepKind::Scalar);
  CHECK(step.targetBits == 32);
  CHECK(step.targetSigned);

  // A source kind BuildResultStep doesn't model yet (tuple) traps rather
  // than crashing.
  ComponentTypeVector tupleItems;
  CHECK(tupleItems.append(ComponentType::primitive(ComponentTypeKind::S32)));
  CHECK(tupleItems.append(ComponentType::primitive(ComponentTypeKind::S32)));
  ComponentType tupleType;
  CHECK(ComponentType::tuple(std::move(tupleItems), &tupleType));
  JS::WebIDLType seqType;
  CHECK(JS::WebIDLType::sequence(longType, &seqType));
  CoerceStep seqStep;
  CHECK(BuildResultStep(cx, seqType, tupleType, &seqStep));
  CHECK(seqStep.kind == StepKind::Trap);
  return true;
}
END_TEST(testBuildResultStep_scalar)

#endif  // ENABLE_WASM_COMPONENTS
