/*
 * Copyright 2026 Mozilla Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "wasm/WasmItasca.h"

#include "js/Printf.h"
#include "wasm/WasmConstants.h"
#include "wasm/WasmGenerator.h"
#include "wasm/WasmMemory.h"
#include "wasm/WasmValidate.h"

using namespace js;
using namespace js::wasm;

using mozilla::Maybe;
using mozilla::Nothing;
using mozilla::Some;

// A rejection message in the shape Decoder::fail produces, so that a message
// from here reads the same as one from the rest of the engine. A failed
// allocation leaves *error null, which every caller of Validate reads as
// out-of-memory.
static bool FailAt(UniqueChars* error, size_t offset, const char* msg) {
  *error = UniqueChars(JS_smprintf("at offset %zu: %s", offset, msg));
  return false;
}

bool wasm::FailItasca(UniqueChars* error, const char* msg) {
  *error = UniqueChars(JS_smprintf("%s", msg));
  return false;
}

static bool FailLimit(UniqueChars* error, const char* what) {
  *error = UniqueChars(JS_smprintf("too many %s", what));
  return false;
}

// itasca's completeness theorems carry VW_MAX_MODULE_BYTES as a hypothesis on
// the whole module, but nothing on itasca's side enforces it. DecodePreamble
// enforces it for a module with a code section, by construction (see
// BytecodeSource::BytecodeSource): reaching either entry point below with a
// module that has none means the length is still unchecked.
static_assert(VW_MAX_MODULE_BYTES == MaxModuleBytes);

static bool CheckModuleLength(size_t length, UniqueChars* error) {
  if (length > MaxModuleBytes) {
    return FailItasca(error, "module too big");
  }
  return true;
}

// --- The caps itasca does not carry ---

static LimitsVisitorState* Self(const VwHookCtx* c) {
  return static_cast<LimitsVisitorState*>(c->user);
}

static uint32_t LimitsOnFunctionStart(const VwHookCtx* c, const VwContext* ctx,
                                      uint32_t, size_t, size_t) {
  // MaxLocals counts a function's parameters, where itasca's MAX_LOCALS caps
  // only what the function declares. VwContext's locals are the parameters
  // followed by the declarations, which is the number MaxLocals bounds.
  if (itasca_context_num_locals(ctx) > MaxLocals) {
    Self(c)->decline = DeclineReason::TooManyLocals;
    return VW_VISIT_OOM;
  }
  return VW_VISIT_OK;
}

static uint32_t LimitsOnBrTableLabel(const VwHookCtx* c, const VwState*,
                                     uint32_t) {
  LimitsVisitorState* self = Self(c);
  self->brTableElems++;
  if (self->brTableElems > MaxBrTableElems) {
    self->decline = DeclineReason::BrTableTooBig;
    return VW_VISIT_OOM;
  }
  return VW_VISIT_OK;
}

static uint32_t LimitsOnBrTable(const VwHookCtx* c, const VwState*, uint32_t,
                                VwBlockType) {
  Self(c)->brTableElems = 0;
  return VW_VISIT_OK;
}

void wasm::InitLimitsVisitor(VwVisitor* visitor) {
  *visitor = VwVisitor{};
  visitor->on_function_start = LimitsOnFunctionStart;
  visitor->on_br_table_label = LimitsOnBrTableLabel;
  visitor->on_br_table = LimitsOnBrTable;
}

void wasm::AssertVisitorIsComplete(const VwVisitor& visitor) {
  // The struct is function pointers and nothing else, which itasca.h's own
  // static_asserts pin, so reading it as words finds every slot without naming
  // each of the 174.
  static_assert(sizeof(VwVisitor) % sizeof(void*) == 0);
  const uintptr_t* slots = reinterpret_cast<const uintptr_t*>(&visitor);
  for (size_t i = 0; i < sizeof(VwVisitor) / sizeof(void*); i++) {
    MOZ_RELEASE_ASSERT(slots[i] != 0, "a itasca hook was left uninstalled");
  }
}

bool wasm::CheckEnvLimits(const VwEnv* env, UniqueChars* error) {
  size_t numTypes = itasca_env_num_types(env);
  if (numTypes > MaxTypes) {
    return FailLimit(error, "types");
  }
  if (itasca_env_num_funcs(env) > MaxFuncs) {
    return FailLimit(error, "functions");
  }
  if (itasca_env_num_imports(env) > MaxImports) {
    return FailLimit(error, "imports");
  }
  if (itasca_env_num_exports(env) > MaxExports) {
    return FailLimit(error, "exports");
  }
  if (itasca_env_num_globals(env) > MaxGlobals) {
    return FailLimit(error, "globals");
  }

  // FuncType has a MOZ_RELEASE_ASSERT on MaxParams, so a wider type would
  // crash the engine rather than be rejected by it.
  for (size_t i = 0; i < numTypes; i++) {
    if (itasca_env_type_num_params(env, i) > MaxParams) {
      return FailLimit(error, "function parameters");
    }
  }

  size_t numElems = itasca_env_num_elements(env);
  if (numElems > MaxElemSegments) {
    return FailLimit(error, "elem segments");
  }
  for (size_t i = 0; i < numElems; i++) {
    if (itasca_env_element_num_funcs(env, i) > MaxElemSegmentLength) {
      return FailLimit(error, "elem segment elements");
    }
  }

  return true;
}

bool wasm::CheckTailLimits(const VwTail* tail, UniqueChars* error) {
  size_t numData = itasca_tail_num_data(tail);
  if (numData > MaxDataSegments) {
    return FailLimit(error, "data segments");
  }

  for (size_t i = 0; i < numData; i++) {
    size_t length = 0;
    (void)itasca_tail_data_bytes(tail, i, &length);
    if (length > uint64_t(MaxDataSegmentLengthPages) * StandardPageSizeBytes) {
      *error = UniqueChars(JS_smprintf("segment size too big"));
      return false;
    }
  }

  return true;
}

// --- Reporting a rejection ---

bool wasm::ReportItascaError(VwStatus status, UniqueChars* error) {
  MOZ_ASSERT(status.error != VW_OK);

  const char* what = itasca_error_str(status.error);
  if (status.detail != 0) {
    *error =
        UniqueChars(JS_smprintf("%s %" PRIu32, what, status.detail));
    return false;
  }
  return FailItasca(error, what);
}

bool wasm::ReportItascaBodyError(VwStatus status, size_t offset,
                                   DeclineReason decline,
                                   UniqueChars* error) {
  MOZ_ASSERT(status.error != VW_OK);

  // A declining hook is reported by itasca as a refusal and nothing more, so
  // the reason is ours to supply.
  switch (decline) {
    case DeclineReason::None:
      break;
    case DeclineReason::OutOfMemory:
      *error = nullptr;
      return false;
    case DeclineReason::TooManyLocals:
      return FailAt(error, offset, "too many locals");
    case DeclineReason::BrTableTooBig:
      return FailAt(error, offset, "br_table too big");
  }

  const char* what = itasca_error_str(status.error);
  if (status.op_error != 0) {
    const char* why = itasca_op_error_str(status.op_error);
    *error =
        UniqueChars(JS_smprintf("at offset %zu: %s: %s", offset, what, why));
    return false;
  }
  return FailAt(error, offset, what);
}

// --- Translating a validated environment into SpiderMonkey's metadata ---

static ValType ToValType(VwValType type) {
  // The enum values are the binary format's, which are TypeCode's, so this is
  // a check rather than a conversion.
  switch (type) {
    case VwValType::I32:
      return ValType::I32;
    case VwValType::I64:
      return ValType::I64;
    case VwValType::F32:
      return ValType::F32;
    case VwValType::F64:
      return ValType::F64;
  }
  MOZ_CRASH("bad value type from itasca");
}

ValType wasm::ToWasmValType(VwValType type) { return ToValType(type); }

BlockType wasm::ToWasmBlockType(VwBlockType type) {
  switch (type) {
    case VwBlockType::Empty:
      return BlockType::VoidToVoid();
    case VwBlockType::I32:
      return BlockType::VoidToSingle(ValType::I32);
    case VwBlockType::I64:
      return BlockType::VoidToSingle(ValType::I64);
    case VwBlockType::F32:
      return BlockType::VoidToSingle(ValType::F32);
    case VwBlockType::F64:
      return BlockType::VoidToSingle(ValType::F64);
  }
  MOZ_CRASH("bad block type from itasca");
}

ValType wasm::ToWasmValType(VwStackType type) {
  switch (type) {
    case VwStackType::I32:
      return ValType::I32;
    case VwStackType::I64:
      return ValType::I64;
    case VwStackType::F32:
      return ValType::F32;
    case VwStackType::F64:
      return ValType::F64;
    case VwStackType::Bot:
      break;
  }
  MOZ_CRASH("stack type is unconstrained in unreachable code");
}

static Limits ToLimits(VwLimits limits) {
  Limits out;
  out.addressType = AddressType::I32;
  out.initial = limits.min;
  out.maximum = limits.has_max ? Some(uint64_t(limits.max)) : Nothing();
  out.shared = Shareable::False;
  out.pageSize = PageSize::Standard;
  return out;
}

static TableType ToTableType(VwTableType table) {
  // wasm 1.0 has one element type, so itasca does not carry it.
  return TableType(ToLimits(table.limits), RefType::func());
}

static GlobalType ToGlobalType(VwGlobalType global) {
  return GlobalType(ToValType(global.valtype),
                    global.mutability == VwMut::Var);
}

static bool ToName(const uint8_t* bytes, size_t length, CacheableName* name) {
  return CacheableName::fromUTF8Bytes(
      mozilla::Span<const char>(reinterpret_cast<const char*>(bytes), length),
      name);
}

// A wasm 1.0 constant expression is exactly one instruction, so itasca hands
// back the value rather than a byte range.
static bool ToInitExpr(VwConstExpr expr, ValType expected, InitExpr* out) {
  switch (expr.kind) {
    case VwConstExprKind::I32:
      return InitExpr::fromLiteral(LitVal(uint32_t(expr.bits)), out);
    case VwConstExprKind::I64:
      return InitExpr::fromLiteral(LitVal(uint64_t(expr.bits)), out);
    case VwConstExprKind::F32: {
      float f;
      uint32_t bits = uint32_t(expr.bits);
      memcpy(&f, &bits, sizeof(f));
      return InitExpr::fromLiteral(LitVal(f), out);
    }
    case VwConstExprKind::F64: {
      double d;
      uint64_t bits = expr.bits;
      memcpy(&d, &bits, sizeof(d));
      return InitExpr::fromLiteral(LitVal(d), out);
    }
    case VwConstExprKind::GlobalGet:
      return InitExpr::fromGlobalGet(expected, uint32_t(expr.bits), out);
  }
  MOZ_CRASH("bad constant expression from itasca");
}

static bool BuildTypes(const VwEnv* env, CodeMetadata* codeMeta) {
  size_t numTypes = itasca_env_num_types(env);
  for (size_t i = 0; i < numTypes; i++) {
    // wasm 1.0 has no recursion groups, so every type is its own group of one.
    MutableRecGroup recGroup = codeMeta->types->startRecGroup(1);
    if (!recGroup) {
      return false;
    }

    ValTypeVector args;
    size_t numArgs = itasca_env_type_num_params(env, i);
    if (!args.reserve(numArgs)) {
      codeMeta->types->cancelStartRecGroup();
      return false;
    }
    for (size_t j = 0; j < numArgs; j++) {
      args.infallibleAppend(ToValType(itasca_env_type_param(env, i, j)));
    }

    ValTypeVector results;
    size_t numResults = itasca_env_type_num_results(env, i);
    if (!results.reserve(numResults)) {
      codeMeta->types->cancelStartRecGroup();
      return false;
    }
    for (size_t j = 0; j < numResults; j++) {
      results.infallibleAppend(ToValType(itasca_env_type_result(env, i, j)));
    }

    TypeDef* typeDef = &recGroup->type(0);
    *typeDef = FuncType(std::move(args), std::move(results));
    typeDef->setFinal(true);
    typeDef->funcType().initImmediateTypeId(/*final=*/true,
                                            /*superType=*/nullptr,
                                            /*recGroupLength=*/1);

    if (!codeMeta->types->endRecGroup()) {
      return false;
    }
  }
  return true;
}

static bool BuildImports(const VwEnv* env, CodeMetadata* codeMeta,
                         ModuleMetadata* moduleMeta) {
  size_t numImports = itasca_env_num_imports(env);
  size_t numTables = 0;
  size_t numMemories = 0;

  for (size_t i = 0; i < numImports; i++) {
    size_t moduleLength = 0;
    const uint8_t* moduleBytes =
        itasca_env_import_module(env, i, &moduleLength);
    size_t fieldLength = 0;
    const uint8_t* fieldBytes = itasca_env_import_name(env, i, &fieldLength);

    CacheableName moduleName;
    CacheableName fieldName;
    if (!ToName(moduleBytes, moduleLength, &moduleName) ||
        !ToName(fieldBytes, fieldLength, &fieldName)) {
      return false;
    }

    VwExternKind kind = itasca_env_import_kind(env, i);
    if (!moduleMeta->imports.emplaceBack(std::move(moduleName),
                                         std::move(fieldName),
                                         DefinitionKind(kind))) {
      return false;
    }

    switch (kind) {
      case VwExternKind::Func:
        if (!codeMeta->funcs.append(
                FuncDesc(itasca_env_import_func_type_index(env, i)))) {
          return false;
        }
        break;
      case VwExternKind::Table:
        if (!codeMeta->tables.emplaceBack(
                ToTableType(itasca_env_import_table(env, i)), Nothing(),
                /*isImported=*/true)) {
          return false;
        }
        numTables++;
        break;
      case VwExternKind::Memory:
        if (!codeMeta->memories.emplaceBack(
                MemoryDesc(ToLimits(itasca_env_import_memory(env, i).limits)))) {
          return false;
        }
        codeMeta->memories.back().importIndex = Some(uint32_t(i));
        numMemories++;
        break;
      case VwExternKind::Global:
        if (!codeMeta->globals.append(
                GlobalDesc(ToGlobalType(itasca_env_import_global(env, i)),
                           codeMeta->globals.length()))) {
          return false;
        }
        break;
    }
  }

  codeMeta->numFuncImports = codeMeta->funcs.length();
  if (!codeMeta->knownFuncImports.resize(codeMeta->numFuncImports)) {
    return false;
  }
  codeMeta->numGlobalImports = codeMeta->globals.length();

  // Defined tables and memories, which itasca indexes as one space with the
  // imported ones.
  for (size_t i = numTables; i < itasca_env_num_tables(env); i++) {
    if (!codeMeta->tables.emplaceBack(ToTableType(itasca_env_table(env, i)),
                                      Nothing(), /*isImported=*/false)) {
      return false;
    }
  }
  for (size_t i = numMemories; i < itasca_env_num_memories(env); i++) {
    if (!codeMeta->memories.emplaceBack(
            MemoryDesc(ToLimits(itasca_env_memory(env, i).limits)))) {
      return false;
    }
  }

  return true;
}

static bool BuildFuncsAndGlobals(const VwEnv* env, CodeMetadata* codeMeta) {
  size_t numFuncDefs = itasca_env_num_func_defs(env);
  if (!codeMeta->funcs.reserve(codeMeta->funcs.length() + numFuncDefs)) {
    return false;
  }
  for (size_t i = 0; i < numFuncDefs; i++) {
    codeMeta->funcs.infallibleAppend(
        FuncDesc(itasca_env_func_def_type_index(env, i)));
  }

  size_t numDefinedGlobals = itasca_env_num_defined_globals(env);
  for (size_t i = 0; i < numDefinedGlobals; i++) {
    GlobalType type = ToGlobalType(itasca_env_defined_global_type(env, i));
    InitExpr initial;
    if (!ToInitExpr(itasca_env_defined_global_init(env, i), type.type,
                    &initial)) {
      return false;
    }
    if (!codeMeta->globals.append(
            GlobalDesc(std::move(initial), type.isMutable))) {
      return false;
    }
  }

  return true;
}

static bool BuildExportsAndStart(const VwEnv* env, CodeMetadata* codeMeta,
                                 ModuleMetadata* moduleMeta) {
  size_t numExports = itasca_env_num_exports(env);
  for (size_t i = 0; i < numExports; i++) {
    size_t nameLength = 0;
    const uint8_t* nameBytes = itasca_env_export_name(env, i, &nameLength);
    CacheableName fieldName;
    if (!ToName(nameBytes, nameLength, &fieldName)) {
      return false;
    }

    uint32_t index = itasca_env_export_index(env, i);
    VwExternKind kind = itasca_env_export_kind(env, i);
    switch (kind) {
      case VwExternKind::Func:
        codeMeta->funcs[index].declareFuncExported(/*eager=*/true,
                                                   /*canRefFunc=*/true);
        break;
      case VwExternKind::Table:
        codeMeta->tables[index].isExported = true;
        break;
      case VwExternKind::Memory:
        break;
      case VwExternKind::Global:
        codeMeta->globals[index].setIsExport();
        break;
    }

    if (!moduleMeta->exports.emplaceBack(std::move(fieldName), index,
                                         DefinitionKind(kind))) {
      return false;
    }
  }

  if (itasca_env_has_start(env)) {
    uint32_t funcIndex = itasca_env_start(env);
    codeMeta->funcs[funcIndex].declareFuncExported(/*eager=*/true,
                                                   /*canRefFunc=*/false);
    codeMeta->startFuncIndex = Some(funcIndex);
  }

  return true;
}

static bool BuildElemSegments(const VwEnv* env, CodeMetadata* codeMeta,
                              ModuleMetadata* moduleMeta) {
  size_t numSegments = itasca_env_num_elements(env);
  if (!moduleMeta->elemSegments.reserve(numSegments) ||
      !codeMeta->elemSegmentTypes.reserve(numSegments)) {
    return false;
  }

  for (size_t i = 0; i < numSegments; i++) {
    ModuleElemSegment seg;
    seg.kind = ModuleElemSegment::Kind::Active;
    seg.tableIndex = itasca_env_element_table_index(env, i);
    seg.elemType = RefType::func().asNonNullable();
    seg.encoding = ModuleElemSegment::Encoding::Indices;

    InitExpr offset;
    if (!ToInitExpr(itasca_env_element_offset(env, i), ValType::I32,
                    &offset)) {
      return false;
    }
    seg.offsetIfActive.emplace(std::move(offset));

    size_t numElems = itasca_env_element_num_funcs(env, i);
    if (!seg.elemIndices.reserve(numElems)) {
      return false;
    }
    for (size_t j = 0; j < numElems; j++) {
      uint32_t funcIndex = itasca_env_element_func(env, i, j);
      seg.elemIndices.infallibleAppend(funcIndex);
      codeMeta->funcs[funcIndex].declareFuncExported(/*eager=*/false,
                                                     /*canRefFunc=*/true);
    }

    codeMeta->elemSegmentTypes.infallibleAppend(seg.elemType);
    moduleMeta->elemSegments.infallibleAppend(std::move(seg));
  }

  return true;
}

// --- Custom sections ---

// What OnCustomSection needs: the region whose decode is running, since the
// hook's positions index the bytes it was handed and a CustomSectionRange has
// to be module-absolute, and where to file what it finds.
struct CustomSectionVisitorState {
  const BytecodeRegion& region;
  CodeMetadata* codeMeta;
  CustomSections* customs;
};

// One custom section, reported by decode_env_with or decode_tail_with, in the
// order it appears. The name range is the bytes immediately before the
// payload, which is the only place a name can be:
// [payload_start - name_len, payload_start). itasca reports a length rather
// than a start there on purpose, since a start would need a subtraction in
// verified code, which is a panic site for something the consumer can do
// itself.
static uint32_t OnCustomSection(void* user, size_t nameLen,
                                size_t payloadStart, size_t payloadEnd) {
  auto* state = static_cast<CustomSectionVisitorState*>(user);
  const BytecodeRegion& region = state->region;

  CustomSectionRange range;
  range.name = BytecodeRange(region.moduleOffset(payloadStart - nameLen),
                             uint32_t(nameLen));
  range.payload = BytecodeRange(region.moduleOffset(payloadStart),
                                uint32_t(payloadEnd - payloadStart));
  if (!state->codeMeta->customSectionRanges.append(range)) {
    return VW_VISIT_OOM;
  }

  // The first custom section named "name", across both regions in module
  // order.
  const uint8_t* nameBytes = region.data() + (payloadStart - nameLen);
  if (state->customs->nameSectionIndex.isNothing() &&
      nameLen == strlen(NameSectionName) &&
      memcmp(nameBytes, NameSectionName, nameLen) == 0) {
    state->customs->nameSectionIndex =
        Some(uint32_t(state->codeMeta->customSectionRanges.length() - 1));
    // The payload runs to the end of its region rather than to the end of
    // the section, so that a subsection reading past its section is caught
    // and reported by finishCustomSection the way it always was, rather than
    // looking like a truncated module.
    state->customs->nameSectionBytes = Some(region.span.From(payloadStart));
  }

  return VW_VISIT_OK;
}

void wasm::DecodeNameSectionWithItasca(const CustomSections& customs,
                                         ModuleMetadata* moduleMeta,
                                         UniqueCharsVector* warnings) {
  if (!customs.nameSectionIndex) {
    return;
  }
  CodeMetadata* codeMeta = moduleMeta->codeMeta;
  DecodeNameSectionPayload(*customs.nameSectionBytes,
                           *customs.nameSectionIndex, codeMeta, moduleMeta,
                           warnings);
}

bool wasm::DecodeEnvWithItasca(const BytecodeRegion& env,
                                 ModuleMetadata* moduleMeta,
                                 CustomSections* customs, UniqueVwEnv* outEnv,
                                 size_t* outCodePos, UniqueChars* error) {
  if (!CheckModuleLength(env.size(), error)) {
    return false;
  }

  CodeMetadata* codeMeta = moduleMeta->codeMeta;
  CustomSectionVisitorState state{env, codeMeta, customs};
  VwModuleVisitor visitor{};
  visitor.on_custom_section = OnCustomSection;

  VwEnv* raw = nullptr;
  VwStatus s = itasca_decode_env_with(env.data(), env.size(), &visitor,
                                        &state, &raw, outCodePos);
  outEnv->reset(raw);
  if (s.error == VW_ERR_VISITOR) {
    // The only way OnCustomSection declines is running out of memory.
    return false;
  }
  if (s.error != VW_OK) {
    return ReportItascaError(s, error);
  }

  const VwEnv* vwEnv = outEnv->get();
  if (!CheckEnvLimits(vwEnv, error)) {
    return false;
  }

  return BuildTypes(vwEnv, codeMeta) &&
         BuildImports(vwEnv, codeMeta, moduleMeta) &&
         BuildFuncsAndGlobals(vwEnv, codeMeta) &&
         BuildExportsAndStart(vwEnv, codeMeta, moduleMeta) &&
         BuildElemSegments(vwEnv, codeMeta, moduleMeta);
}

static bool BuildDataSegments(const VwTail* tail, ModuleMetadata* moduleMeta) {
  size_t numSegments = itasca_tail_num_data(tail);
  if (!moduleMeta->dataSegments.reserve(numSegments)) {
    return false;
  }

  for (size_t i = 0; i < numSegments; i++) {
    MutableDataSegment seg = js_new<DataSegment>();
    if (!seg) {
      return false;
    }
    seg->memoryIndex = itasca_tail_data_memory_index(tail, i);

    InitExpr offset;
    if (!ToInitExpr(itasca_tail_data_offset(tail, i), ValType::I32,
                    &offset)) {
      return false;
    }
    seg->offsetIfActive.emplace(std::move(offset));

    size_t length = 0;
    const uint8_t* bytes = itasca_tail_data_bytes(tail, i, &length);
    if (!seg->bytes.append(bytes, length)) {
      return false;
    }

    moduleMeta->dataSegments.infallibleAppend(std::move(seg));
  }

  return true;
}

bool wasm::DecodeTailWithItasca(const BytecodeRegion& tail, const VwEnv* env,
                                  ModuleMetadata* moduleMeta,
                                  CustomSections* customs,
                                  UniqueChars* error) {
  CodeMetadata* codeMeta = moduleMeta->codeMeta;
  CustomSectionVisitorState state{tail, codeMeta, customs};
  VwModuleVisitor visitor{};
  visitor.on_custom_section = OnCustomSection;

  UniqueVwTail decoded;
  {
    VwTail* raw = nullptr;
    VwStatus s = itasca_decode_tail_with(tail.data(), tail.size(), 0, env,
                                           &visitor, &state, &raw);
    decoded.reset(raw);
    if (s.error == VW_ERR_VISITOR) {
      // The only way OnCustomSection declines is running out of memory.
      return false;
    }
    if (s.error != VW_OK) {
      return ReportItascaError(s, error);
    }
  }

  return CheckTailLimits(decoded.get(), error) &&
         BuildDataSegments(decoded.get(), moduleMeta);
}

// --- The code section ---

// itasca's own cap on a function body (MAX_FUNCTION_BYTES) is on the bytes
// after the locals declarations, and matches SM's MaxFunctionBytes; SM's is
// the one the engine's structures assume, so it is checked on this side
// against the whole entry, declarations included.
static bool CheckEntryFits(size_t contentsStart, size_t entryEnd,
                           UniqueChars* error) {
  if (entryEnd - contentsStart > MaxFunctionBytes) {
    return FailItasca(error, "function body too big");
  }
  return true;
}

// What CompileOnNeedBytes and CompileOnCodeEntry need to discharge the
// obligation itasca_validate_code_with hands them: each entry has to reach
// mg.compileFuncDef, which is what validates it, on some thread, before
// mg.finishFuncDefs() is allowed to run.
struct CompileCodeState {
  const BytecodeRegion& code;
  const CodeMetadata& codeMeta;
  ModuleGenerator& mg;
  CodeSectionArrival& arrival;
  UniqueChars* error;
};

static uint32_t CompileOnNeedBytes(void* user, size_t end) {
  auto* state = static_cast<CompileCodeState*>(user);
  // A cancelled stream has already set *error.
  return state->arrival.waitFor(end) ? VW_VISIT_OK : VW_VISIT_OOM;
}

static uint32_t CompileOnCodeEntry(void* user, size_t index, size_t entryPos,
                                   size_t contentsStart, size_t entryEnd) {
  auto* state = static_cast<CompileCodeState*>(user);
  const BytecodeRegion& code = state->code;

  if (!CheckEntryFits(contentsStart, entryEnd, state->error)) {
    return VW_VISIT_OOM;
  }
  // The entry is about to be handed to another thread, which reads it
  // without validating it: mg.compileFuncDef's worker drives itasca over it
  // itself, and mg.finishFuncDefs() fails the compile if that rejects.
  if (!state->mg.compileFuncDef(
          state->codeMeta.numFuncImports + index,
          code.moduleOffset(contentsStart), uint32_t(entryPos),
          code.data() + contentsStart, code.data() + entryEnd)) {
    return VW_VISIT_OOM;
  }
  return VW_VISIT_OK;
}

bool wasm::DecodeCodeSectionWithItasca(const BytecodeRegion& code,
                                         const VwEnv* env,
                                         const CodeMetadata& codeMeta,
                                         ModuleGenerator& mg,
                                         CodeSectionArrival& arrival,
                                         UniqueChars* error) {
  CompileCodeState state{code, codeMeta, mg, arrival, error};
  VwCodeVisitor visitor{};
  visitor.on_need_bytes = CompileOnNeedBytes;
  visitor.on_code_entry = CompileOnCodeEntry;

  size_t next = 0;
  VwStatus s = itasca_validate_code_with(code.data(), code.size(), 0, env,
                                           &visitor, &state, &next);
  if (s.error == VW_ERR_VISITOR) {
    // CompileOnNeedBytes or CompileOnCodeEntry declined and has already said
    // why, or left *error null for out-of-memory.
    return false;
  }
  if (s.error != VW_OK) {
    return ReportItascaError(s, error);
  }

  // itasca showed the entries tile the section; the region is the section
  // here, so showing the section fills the region is this side's to check. A
  // split that put anything else in here is one itasca did not agree with.
  if (next != code.size()) {
    return FailItasca(error, "code section size does not match its contents");
  }

  return mg.finishFuncDefs();
}

static uint32_t RefuseCodeEntry(void*, size_t, size_t, size_t, size_t) {
  return VW_VISIT_OOM;
}

bool wasm::CheckNoCodeSectionWithItasca(const BytecodeRegion& region,
                                          const VwEnv* env,
                                          UniqueChars* error) {
  VwCodeVisitor visitor{};
  visitor.on_code_entry = RefuseCodeEntry;

  size_t next = 0;
  VwStatus s = itasca_validate_code_with(region.data(), region.size(), 0,
                                           env, &visitor, nullptr, &next);
  if (s.error == VW_ERR_VISITOR || (s.error == VW_OK && next != 0)) {
    // The splitter passed over a code section larger than
    // MaxCodeSectionBytes, or with a header its own read did not accept, which
    // is the only way to be here with itasca finding one: either its first
    // entry declined, or it found no entries and read the header alone.
    return FailItasca(error, "too many code section bytes");
  }
  if (s.error != VW_OK) {
    return ReportItascaError(s, error);
  }
  return true;
}

// --- Validation ---

bool wasm::DriveBodyWithItasca(const BytecodeRegion& code,
                                 size_t entryOffset, const VwEnv* env,
                                 uint32_t funcDefIndex,
                                 const VwVisitor& visitor, void* user,
                                 const DeclineReason* decline,
                                 UniqueChars* error) {
  size_t next = 0;
  size_t errorOffset = 0;
  VwStatus s = itasca_validate_code_entry(code.data(), code.size(),
                                            entryOffset, env, funcDefIndex,
                                            &visitor, user, &next,
                                            &errorOffset);
  if (s.error != VW_OK) {
    // A hook that refused because the compiler refused has already said why,
    // through the decoder it shares with the rest of the engine.
    if (*error) {
      return false;
    }
    return ReportItascaBodyError(s, code.moduleOffset(errorOffset), *decline,
                                   error);
  }
  return true;
}

bool wasm::DriveBodyWithItascaBaseline(const BytecodeRegion& code,
                                         size_t entryOffset, const VwEnv* env,
                                         uint32_t funcDefIndex, void* user,
                                         const DeclineReason* decline,
                                         UniqueChars* error) {
  size_t next = 0;
  size_t errorOffset = 0;
  VwStatus s = itasca_validate_code_entry_baseline(
      code.data(), code.size(), entryOffset, env, funcDefIndex, user, &next,
      &errorOffset);
  if (s.error != VW_OK) {
    // A hook that refused because the compiler refused has already said why,
    // through the decoder it shares with the rest of the engine.
    if (*error) {
      return false;
    }
    return ReportItascaBodyError(s, code.moduleOffset(errorOffset), *decline,
                                   error);
  }
  return true;
}


bool wasm::RegionsAfterEnv(const BytecodeSource& bytecode, size_t codePos,
                           BytecodeRegion* code, BytecodeRegion* tail,
                           UniqueChars* error) {
  BytecodeRegion env(bytecode.envSpan(), 0);

  if (!bytecode.hasCodeSection()) {
    // Everything the environment did not take is in its region, and there is
    // no code section for the tail to follow.
    *code = env.from(codePos);
    *tail = *code;
    return true;
  }

  // The split and the environment have to end in the same place. A section
  // between them is one the environment would not take and the split did not
  // reach: the module has it before the code section, where it may not be.
  if (codePos != env.size()) {
    return FailItasca(error, "unknown section before code section");
  }

  *code = BytecodeRegion(bytecode.codeSpan(), bytecode.codeRange().start);
  *tail = BytecodeRegion(bytecode.tailSpan(), bytecode.tailRange().start);
  return true;
}

// What ValidateOnCodeEntry needs to discharge validate_code_with's obligation
// itself, by calling itasca_validate_code_entry against the same entry and
// env it was just handed.
struct ValidateCodeState {
  const BytecodeRegion& code;
  const VwEnv* env;
  const VwVisitor& visitor;
  UniqueChars* error;
};

static uint32_t ValidateOnCodeEntry(void* user, size_t index, size_t entryPos,
                                    size_t contentsStart, size_t entryEnd) {
  auto* state = static_cast<ValidateCodeState*>(user);
  const BytecodeRegion& code = state->code;

  if (!CheckEntryFits(contentsStart, entryEnd, state->error)) {
    return VW_VISIT_OOM;
  }

  LimitsVisitorState limits;
  size_t next = 0;
  size_t errorOffset = 0;
  VwStatus s = itasca_validate_code_entry(code.data(), code.size(), entryPos,
                                            state->env, index, &state->visitor,
                                            &limits, &next, &errorOffset);
  if (s.error != VW_OK) {
    return ReportItascaBodyError(s, code.moduleOffset(errorOffset),
                                   limits.decline, state->error)
               ? VW_VISIT_OK
               : VW_VISIT_OOM;
  }
  return VW_VISIT_OK;
}

// The code-section half of ValidateWithItasca, split out because it is the
// one part CheckNoCodeSectionWithItasca does not cover.
static bool ValidateCodeSectionWithItasca(const BytecodeRegion& code,
                                            const VwEnv* env,
                                            UniqueChars* error) {
  VwVisitor visitor;
  InitLimitsVisitor(&visitor);

  ValidateCodeState state{code, env, visitor, error};
  VwCodeVisitor codeVisitor{};
  codeVisitor.on_code_entry = ValidateOnCodeEntry;

  size_t next = 0;
  VwStatus s = itasca_validate_code_with(code.data(), code.size(), 0, env,
                                           &codeVisitor, &state, &next);
  if (s.error == VW_ERR_VISITOR) {
    // ValidateOnCodeEntry declined and has already reported why, or left
    // *error null for out-of-memory.
    return false;
  }
  if (s.error != VW_OK) {
    return ReportItascaError(s, error);
  }

  // itasca showed the entries tile the section; the region is the section
  // here, so showing the section fills the region is this side's to check.
  if (next != code.size()) {
    return FailAt(error, code.moduleOffset(next),
                 "code section size does not match its contents");
  }
  return true;
}

bool wasm::ValidateWithItasca(const BytecodeSource& bytecode,
                                UniqueChars* error) {
  BytecodeSpan envSpan = bytecode.envSpan();

  if (!CheckModuleLength(envSpan.size(), error)) {
    return false;
  }

  UniqueVwEnv env;
  size_t codePos = 0;
  {
    VwEnv* raw = nullptr;
    VwStatus s =
        itasca_decode_env(envSpan.data(), envSpan.size(), &raw, &codePos);
    env.reset(raw);
    if (s.error != VW_OK) {
      return ReportItascaError(s, error);
    }
  }

  if (!CheckEnvLimits(env.get(), error)) {
    return false;
  }

  BytecodeRegion code;
  BytecodeRegion tail;
  if (!RegionsAfterEnv(bytecode, codePos, &code, &tail, error)) {
    return false;
  }

  if (!bytecode.hasCodeSection()) {
    if (!CheckNoCodeSectionWithItasca(code, env.get(), error)) {
      return false;
    }
  } else if (!ValidateCodeSectionWithItasca(code, env.get(), error)) {
    return false;
  }

  UniqueVwTail decoded;
  {
    VwTail* raw = nullptr;
    VwStatus s =
        itasca_decode_tail(tail.data(), tail.size(), 0, env.get(), &raw);
    decoded.reset(raw);
    if (s.error != VW_OK) {
      return ReportItascaError(s, error);
    }
  }

  return CheckTailLimits(decoded.get(), error);
}
