/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "wasm/WasmComponentAdapter.h"

#ifdef ENABLE_WASM_COMPONENTS

#  include "mozilla/Casting.h"

#  include "builtin/String.h"
#  include "js/CallAndConstruct.h"
#  include "js/Conversions.h"
#  include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#  include "util/Unicode.h"
#  include "vm/BigIntType.h"
#  include "vm/ErrorObject.h"
#  include "vm/JSFunction.h"
#  include "wasm/WasmBinary.h"
#  include "wasm/WasmBuiltinModule.h"
#  include "wasm/WasmBuiltins.h"
#  include "wasm/WasmFeatures.h"
#  include "wasm/WasmGenerator.h"
#  include "wasm/WasmInstance.h"
#  include "wasm/WasmJS.h"
#  include "wasm/WasmValidate.h"

#  include "vm/JSObject-inl.h"
#  include "wasm/WasmInstance-inl.h"

using namespace js;
using namespace js::wasm;

// Reasons for CmTrap.
enum class AdapterTrap : int32_t {
  UncaughtException,
  ImportException,
};

// Every adapter module imports its WasmComponentInstanceObject as this global.
static constexpr uint32_t CtxGlobalIndex = 0;

namespace {

// Builds and instantiates a module whose only defined function is an adapter,
// along with the imports it needs.
class MOZ_STACK_CLASS AdapterModuleBuilder {
  JSContext* cx_;
  MutableModuleMetadata moduleMeta_;
  MutableCodeMetadata codeMeta_;
  Rooted<ImportValues> imports_;
  uint32_t adapterFuncIndex_ = UINT32_MAX;

 public:
  explicit AdapterModuleBuilder(JSContext* cx) : cx_(cx), imports_(cx) {}

  [[nodiscard]] bool init(Handle<WasmComponentInstanceObject*> instanceObj);

  const TypeContext& types() const { return *codeMeta_->types; }

  // Imports a wasm function with its own type. Function imports must all be
  // added before the adapter function.
  [[nodiscard]] bool importWasmFunc(HandleFunction func, uint32_t* funcIndex);

  // Imports a function that is not a wasm function, which is called like a JS
  // function with this signature.
  [[nodiscard]] bool importJSFunc(HandleObject func, ValTypeVector&& params,
                                  ValTypeVector&& results, uint32_t* funcIndex);

  [[nodiscard]] bool defineAdapterFunc(ValTypeVector&& params,
                                       ValTypeVector&& results);
  [[nodiscard]] bool defineAdapterFuncWithType(const TypeDef& funcType);

  [[nodiscard]] bool finish(const Bytes& body, MutableHandleFunction result);
};

bool AdapterModuleBuilder::init(
    Handle<WasmComponentInstanceObject*> instanceObj) {
  FeatureOptions options;
  options.isBuiltinModule = true;
  SharedCompileArgs compileArgs = CompileArgs::buildAndReport(
      cx_, ScriptedCaller::selfHosted(cx_), options);
  if (!compileArgs) {
    return false;
  }

  moduleMeta_ = js_new<ModuleMetadata>();
  if (!moduleMeta_ || !moduleMeta_->init(*compileArgs)) {
    ReportOutOfMemory(cx_);
    return false;
  }
  codeMeta_ = moduleMeta_->codeMeta;

  MOZ_ASSERT(codeMeta_->globals.length() == CtxGlobalIndex);
  GlobalType ctxType(ValType(RefType::extern_()), /*isMutable=*/false);
  Rooted<Val> ctx(cx_, Val(ValType(RefType::extern_()),
                           AnyRef::fromJSObject(*instanceObj)));
  if (!codeMeta_->globals.append(GlobalDesc(ctxType, /*importIndex=*/0)) ||
      !moduleMeta_->imports.emplaceBack(CacheableName(), CacheableName(),
                                        DefinitionKind::Global) ||
      !imports_.get().globalValues.append(ctx.get())) {
    ReportOutOfMemory(cx_);
    return false;
  }
  return true;
}

bool AdapterModuleBuilder::importWasmFunc(HandleFunction func,
                                          uint32_t* funcIndex) {
  MOZ_ASSERT(func->isWasm());
  MOZ_ASSERT(adapterFuncIndex_ == UINT32_MAX);

  // Import the function with its own type definition, so that linking it is
  // an exact type match.
  const TypeDef* typeDef = func->wasmTypeDef();
  uint32_t typeIndex =
      codeMeta_->types->length() + typeDef->recGroup().indexOf(typeDef);
  *funcIndex = codeMeta_->funcs.length();
  if (!codeMeta_->types->addRecGroup(&typeDef->recGroup()) ||
      !moduleMeta_->addImportedFuncWithType(typeIndex, CacheableName(),
                                            CacheableName()) ||
      !imports_.get().funcs.append(func)) {
    ReportOutOfMemory(cx_);
    return false;
  }
  return true;
}

bool AdapterModuleBuilder::importJSFunc(HandleObject func,
                                        ValTypeVector&& params,
                                        ValTypeVector&& results,
                                        uint32_t* funcIndex) {
  MOZ_ASSERT(!(func->is<JSFunction>() && func->as<JSFunction>().isWasm()));
  MOZ_ASSERT(adapterFuncIndex_ == UINT32_MAX);
  *funcIndex = codeMeta_->funcs.length();
  if (!moduleMeta_->addImportedFunc(std::move(params), std::move(results),
                                    CacheableName(), CacheableName()) ||
      !imports_.get().funcs.append(func)) {
    ReportOutOfMemory(cx_);
    return false;
  }
  return true;
}

bool AdapterModuleBuilder::defineAdapterFuncWithType(const TypeDef& funcType) {
  MOZ_ASSERT(adapterFuncIndex_ == UINT32_MAX);
  MOZ_ASSERT(funcType.recGroup().numTypes() == 1);
  adapterFuncIndex_ = codeMeta_->funcs.length();
  uint32_t typeIndex = codeMeta_->types->length();
  if (!codeMeta_->types->addRecGroup(&funcType.recGroup()) ||
      !moduleMeta_->addDefinedFuncWithType(typeIndex, /*declareForRef=*/true,
                                           mozilla::Some(CacheableName()))) {
    ReportOutOfMemory(cx_);
    return false;
  }
  return true;
}

bool AdapterModuleBuilder::defineAdapterFunc(ValTypeVector&& params,
                                             ValTypeVector&& results) {
  MOZ_ASSERT(adapterFuncIndex_ == UINT32_MAX);
  adapterFuncIndex_ = codeMeta_->funcs.length();
  if (!moduleMeta_->addDefinedFunc(std::move(params), std::move(results),
                                   /*declareForRef=*/true,
                                   mozilla::Some(CacheableName()))) {
    ReportOutOfMemory(cx_);
    return false;
  }
  return true;
}

bool AdapterModuleBuilder::finish(const Bytes& body,
                                  MutableHandleFunction result) {
  MOZ_ASSERT(adapterFuncIndex_ != UINT32_MAX);

  CompilerEnvironment compilerEnv(
      CompileMode::Once,
      BaselineAvailable(cx_) ? Tier::Baseline : Tier::Optimized,
      DebugEnabled::False);
  compilerEnv.computeParameters();

  if (!moduleMeta_->prepareForCompile(compilerEnv.mode())) {
    ReportOutOfMemory(cx_);
    return false;
  }

  UniqueChars error;
  ModuleGenerator mg(*codeMeta_, compilerEnv, compilerEnv.initialState(),
                     nullptr, &error, nullptr);
  SharedModule module;
  if (mg.initializeCompleteTier() &&
      mg.compileFuncDef(adapterFuncIndex_,
                        CallSite::FIRST_VALID_BYTECODE_OFFSET, body.begin(),
                        body.end()) &&
      mg.finishFuncDefs()) {
    module = mg.finishModule(BytecodeBufferOrSource(), *moduleMeta_,
                             /*maybeCompleteTier2Listener=*/nullptr);
  }
  if (!module) {
    if (error) {
      // Adapter code is always valid, so this is a bug.
      MOZ_ASSERT_UNREACHABLE("invalid adapter code");
      JS_ReportErrorASCII(cx_, "invalid component adapter: %s", error.get());
      return false;
    }
    ReportOutOfMemory(cx_);
    return false;
  }

  Rooted<WasmInstanceObject*> instanceObj(cx_);
  if (!module->instantiate(cx_, imports_.get(), nullptr, &instanceObj)) {
    return false;
  }
  return WasmInstanceObject::getExportedFunction(cx_, instanceObj,
                                                 adapterFuncIndex_, result);
}

// Encodes the body of an adapter function.
class AdapterFuncEncoder {
  const TypeContext& types_;
  uint32_t numParams_;
  ValTypeVector locals_;
  Bytes code_;
  Encoder encoder_;

 public:
  AdapterFuncEncoder(const TypeContext& types, uint32_t numParams)
      : types_(types), numParams_(numParams), encoder_(code_, types) {}

  Encoder& encoder() { return encoder_; }

  [[nodiscard]] bool addLocal(ValType type, uint32_t* index) {
    *index = numParams_ + locals_.length();
    return locals_.append(type);
  }

  [[nodiscard]] bool op(Op op) { return encoder_.writeOp(op); }
  [[nodiscard]] bool localGet(uint32_t index) {
    return encoder_.writeOp(Op::LocalGet) && encoder_.writeVarU32(index);
  }
  [[nodiscard]] bool localSet(uint32_t index) {
    return encoder_.writeOp(Op::LocalSet) && encoder_.writeVarU32(index);
  }
  [[nodiscard]] bool ctx() {
    return encoder_.writeOp(Op::GlobalGet) &&
           encoder_.writeVarU32(CtxGlobalIndex);
  }
  [[nodiscard]] bool i32Const(int32_t value) {
    return encoder_.writeOp(Op::I32Const) && encoder_.writeVarS32(value);
  }
  [[nodiscard]] bool call(uint32_t funcIndex) {
    return encoder_.writeOp(Op::Call) && encoder_.writeVarU32(funcIndex);
  }
  [[nodiscard]] bool callBuiltin(BuiltinModuleFuncId id) {
    return encoder_.writeOp(MozOp::CallBuiltinModuleFunc) &&
           encoder_.writeVarU32(uint32_t(id));
  }

  // Begins a block with no params and zero or one result.
  [[nodiscard]] bool block(Op op, mozilla::Maybe<ValType> result) {
    if (!encoder_.writeOp(op)) {
      return false;
    }
    if (result) {
      return encoder_.writeValType(*result);
    }
    return encoder_.writeFixedU8(uint8_t(TypeCode::BlockVoid));
  }

  // Begins a try_table that branches to `depth` on any exception.
  [[nodiscard]] bool tryTableCatchAll(mozilla::Maybe<ValType> result,
                                      uint32_t depth) {
    return block(Op::TryTable, result) && encoder_.writeVarU32(1) &&
           encoder_.writeFixedU8(0x2) && encoder_.writeVarU32(depth);
  }

  [[nodiscard]] bool trap(AdapterTrap reason) {
    return i32Const(int32_t(reason)) &&
           callBuiltin(BuiltinModuleFuncId::CmTrap) && op(Op::Unreachable);
  }

  [[nodiscard]] bool finish(Bytes* body) {
    Encoder encoder(*body, types_);
    return EncodeLocalEntries(encoder, locals_) && body->appendAll(code_) &&
           encoder.writeOp(Op::End);
  }
};

}  // namespace

static bool IsIntegerKind(ComponentTypeKind kind) {
  switch (kind) {
    case ComponentTypeKind::S8:
    case ComponentTypeKind::U8:
    case ComponentTypeKind::S16:
    case ComponentTypeKind::U16:
    case ComponentTypeKind::S32:
    case ComponentTypeKind::U32:
      return true;
    default:
      return false;
  }
}

static bool IsSupportedAdapterType(const ComponentType& type) {
  switch (type.kind()) {
    case ComponentTypeKind::Bool:
    case ComponentTypeKind::S64:
    case ComponentTypeKind::U64:
    case ComponentTypeKind::F32:
    case ComponentTypeKind::F64:
    case ComponentTypeKind::Char:
      return true;
    default:
      return IsIntegerKind(type.kind());
  }
}

[[nodiscard]] static bool CheckSupportedAdapterFuncType(
    JSContext* cx, const ComponentFuncType& funcType) {
  bool supported = funcType.resultType.isNothing() ||
                   IsSupportedAdapterType(*funcType.resultType);
  for (const ComponentType& type : funcType.paramTypes) {
    supported = supported && IsSupportedAdapterType(type);
  }

  ValTypeVector flatParams;
  bool hasStringsOrLists, tooDeep;
  if (!FlattenTypes(funcType.paramTypes, &flatParams, &hasStringsOrLists,
                    &tooDeep, 0)) {
    ReportOutOfMemory(cx);
    return false;
  }
  if (!supported || flatParams.length() > MaxFlatParams) {
    JS_ReportErrorASCII(cx, "component function type is not supported yet");
    return false;
  }
  return true;
}

// The core wasm types a component value is flattened to, stored in locals.
using FlatLocals = Vector<uint32_t, 1, SystemAllocPolicy>;

[[nodiscard]] static bool FlatTypes(const ComponentType& type,
                                    ValTypeVector* flat) {
  bool hasStringsOrLists, tooDeep;
  return FlattenType(type, flat, &hasStringsOrLists, &tooDeep, 0);
}

// Gets the type of the single flat value that a result is returned as.
[[nodiscard]] static bool FlatResultType(const ComponentType& type,
                                         mozilla::Maybe<ValType>* result) {
  ValTypeVector flat;
  if (!FlatTypes(type, &flat)) {
    return false;
  }
  MOZ_RELEASE_ASSERT(flat.length() == 1);
  result->emplace(flat[0]);
  return true;
}

[[nodiscard]] static bool AddFlatLocals(AdapterFuncEncoder& f,
                                        const ComponentType& type,
                                        FlatLocals* locals) {
  ValTypeVector flat;
  if (!FlatTypes(type, &flat)) {
    return false;
  }
  for (ValType flatType : flat) {
    uint32_t index;
    if (!f.addLocal(flatType, &index) || !locals->append(index)) {
      return false;
    }
  }
  return true;
}

// Emits ToComponentValue for the JS value in local `jsValue`, storing the
// flattened component value to `dest`.
[[nodiscard]] static bool EmitToComponentValue(AdapterFuncEncoder& f,
                                               const ComponentType& type,
                                               uint32_t jsValue,
                                               const FlatLocals& dest) {
  using Id = BuiltinModuleFuncId;
  ComponentTypeKind kind = type.kind();
  switch (kind) {
    case ComponentTypeKind::Bool:
      return f.localGet(jsValue) && f.callBuiltin(Id::CmToBool) &&
             f.localSet(dest[0]);
    case ComponentTypeKind::S64:
    case ComponentTypeKind::U64:
      return f.ctx() && f.localGet(jsValue) && f.i32Const(int32_t(kind)) &&
             f.callBuiltin(Id::CmToInteger) && f.ctx() &&
             f.callBuiltin(Id::CmScratchI64) && f.localSet(dest[0]);
    case ComponentTypeKind::F32:
    case ComponentTypeKind::F64:
      return f.ctx() && f.localGet(jsValue) && f.callBuiltin(Id::CmToFloat) &&
             f.ctx() && f.callBuiltin(Id::CmScratchF64) &&
             (kind == ComponentTypeKind::F64 || f.op(Op::F32DemoteF64)) &&
             f.localSet(dest[0]);
    case ComponentTypeKind::Char:
      return f.localGet(jsValue) && f.callBuiltin(Id::CmToChar) &&
             f.localSet(dest[0]);
    default:
      MOZ_ASSERT(IsIntegerKind(kind));
      return f.ctx() && f.localGet(jsValue) && f.i32Const(int32_t(kind)) &&
             f.callBuiltin(Id::CmToInteger) && f.ctx() &&
             f.callBuiltin(Id::CmScratchI32) && f.localSet(dest[0]);
  }
}

// Emits ToJSValue for the flattened component value in `src`, leaving the JS
// value on the stack.
[[nodiscard]] static bool EmitToJSValue(AdapterFuncEncoder& f,
                                        const ComponentType& type,
                                        const FlatLocals& src) {
  using Id = BuiltinModuleFuncId;
  ComponentTypeKind kind = type.kind();
  switch (kind) {
    case ComponentTypeKind::Bool:
      return f.localGet(src[0]) && f.callBuiltin(Id::CmBoolToJS);
    case ComponentTypeKind::S64:
    case ComponentTypeKind::U64:
      return f.localGet(src[0]) && f.i32Const(int32_t(kind)) &&
             f.callBuiltin(Id::CmInt64ToJS);
    case ComponentTypeKind::F32:
      return f.localGet(src[0]) && f.op(Op::F64PromoteF32) &&
             f.callBuiltin(Id::CmFloatToJS);
    case ComponentTypeKind::F64:
      return f.localGet(src[0]) && f.callBuiltin(Id::CmFloatToJS);
    case ComponentTypeKind::Char:
      return f.localGet(src[0]) && f.callBuiltin(Id::CmCharToJS);
    default:
      MOZ_ASSERT(IsIntegerKind(kind));
      return f.localGet(src[0]) && f.i32Const(int32_t(kind)) &&
             f.callBuiltin(Id::CmIntegerToJS);
  }
}

// Generates the adapter for a lifted function, which takes one externref for
// each parameter and returns an externref if the function has a result:
//
//   (func (param externref)* (result externref)?
//     ;; ToComponentValue for each parameter, into flat locals
//     block
//       try_table (result flat-result)? (catch_all 0)
//         (local.get flat-param)*
//         call $callee
//       end
//       ;; ToJSValue for the result, if any
//       return
//     end
//     ;; the callee threw
//     trap
//   )
[[nodiscard]] static bool GenerateLiftAdapter(
    JSContext* cx, Handle<WasmComponentInstanceObject*> instanceObj,
    const ComponentFuncType& funcType, HandleFunction callee,
    MutableHandleFunction result) {
  AdapterModuleBuilder builder(cx);
  uint32_t calleeIndex;
  if (!builder.init(instanceObj) ||
      !builder.importWasmFunc(callee, &calleeIndex)) {
    return false;
  }

  uint32_t numParams = funcType.paramTypes.length();
  ValTypeVector params, results;
  if (!params.appendN(ValType(RefType::extern_()), numParams) ||
      (funcType.resultType && !results.append(ValType(RefType::extern_())))) {
    ReportOutOfMemory(cx);
    return false;
  }
  if (!builder.defineAdapterFunc(std::move(params), std::move(results))) {
    return false;
  }

  AdapterFuncEncoder f(builder.types(), numParams);
  Vector<FlatLocals, 8, SystemAllocPolicy> flatParams;
  if (!flatParams.resize(numParams)) {
    ReportOutOfMemory(cx);
    return false;
  }
  for (uint32_t i = 0; i < numParams; i++) {
    const ComponentType& type = funcType.paramTypes[i];
    if (!AddFlatLocals(f, type, &flatParams[i]) ||
        !EmitToComponentValue(f, type, i, flatParams[i])) {
      ReportOutOfMemory(cx);
      return false;
    }
  }

  FlatLocals flatResult;
  mozilla::Maybe<ValType> flatResultType;
  if (funcType.resultType &&
      (!AddFlatLocals(f, *funcType.resultType, &flatResult) ||
       !FlatResultType(*funcType.resultType, &flatResultType))) {
    ReportOutOfMemory(cx);
    return false;
  }

  bool ok = f.block(Op::Block, mozilla::Nothing()) &&
            f.tryTableCatchAll(flatResultType, 0);
  for (const FlatLocals& locals : flatParams) {
    for (uint32_t local : locals) {
      ok = ok && f.localGet(local);
    }
  }
  ok = ok && f.call(calleeIndex) && f.op(Op::End);
  if (funcType.resultType) {
    ok = ok && f.localSet(flatResult[0]) &&
         EmitToJSValue(f, *funcType.resultType, flatResult);
  }
  ok = ok && f.op(Op::Return) && f.op(Op::End) &&
       f.trap(AdapterTrap::UncaughtException);

  Bytes body;
  if (!ok || !f.finish(&body)) {
    ReportOutOfMemory(cx);
    return false;
  }
  return builder.finish(body, result);
}

enum LiftedFunctionSlots {
  LiftedFunctionSlot_Adapter,
  LiftedFunctionSlot_Instance,
  LiftedFunctionSlot_NumParams,
};

static bool IsPendingTrap(JSContext* cx) {
  RootedValue exn(cx);
  if (!cx->isExceptionPending() || !cx->getPendingException(&exn)) {
    return false;
  }
  return exn.isObject() && exn.toObject().is<ErrorObject>() &&
         exn.toObject().as<ErrorObject>().fromWasmTrap();
}

// The JS-API's "invoke a component function".
static bool CallLiftedFunction(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  JSFunction& callee = args.callee().as<JSFunction>();

  uint32_t numParams =
      callee.getExtendedSlot(LiftedFunctionSlot_NumParams).toInt32();

  ComponentInstance& instance =
      callee.getExtendedSlot(LiftedFunctionSlot_Instance)
          .toObject()
          .as<WasmComponentInstanceObject>()
          .instance();
  if (instance.trapped()) {
    ReportTrapError(cx, JSMSG_WASM_COMPONENT_TRAPPED);
    return false;
  }

  RootedValue adapter(cx, callee.getExtendedSlot(LiftedFunctionSlot_Adapter));
  InvokeArgs adapterArgs(cx);
  if (!adapterArgs.init(cx, numParams)) {
    return false;
  }
  for (uint32_t i = 0; i < numParams; i++) {
    adapterArgs[i].set(args.get(i));
  }

  if (!Call(cx, adapter, UndefinedHandleValue, adapterArgs, args.rval())) {
    if (IsPendingTrap(cx)) {
      instance.setTrapped();
    }
    return false;
  }
  return true;
}

bool wasm::CreateLiftedFunction(
    JSContext* cx, Handle<WasmComponentInstanceObject*> instanceObj,
    const ComponentFuncType& funcType, HandleFunction callee,
    Handle<JSAtom*> name, MutableHandleFunction result) {
  if (!CheckSupportedAdapterFuncType(cx, funcType)) {
    return false;
  }

  RootedFunction adapter(cx);
  if (!GenerateLiftAdapter(cx, instanceObj, funcType, callee, &adapter)) {
    return false;
  }

  uint32_t numParams = funcType.paramTypes.length();
  result.set(NewNativeFunction(cx, CallLiftedFunction, numParams, name,
                               gc::AllocKind::FUNCTION_EXTENDED));
  if (!result) {
    return false;
  }
  result->initExtendedSlot(LiftedFunctionSlot_Adapter, ObjectValue(*adapter));
  result->initExtendedSlot(LiftedFunctionSlot_Instance,
                           ObjectValue(*instanceObj));
  result->initExtendedSlot(LiftedFunctionSlot_NumParams,
                           Int32Value(int32_t(numParams)));
  return true;
}

// Generates the adapter for a lowered function, which has the flattened core
// function type and calls the host function with one externref for each
// parameter:
//
//   (func (param flat-param)* (result flat-result)?
//     ;; ToJSValue for each parameter, into externref locals
//     block
//       try_table (result flat-result)? (catch_all 0)
//         (local.get js-param)*
//         call $host
//         ;; ToComponentValue for the result, if any
//       end
//       return
//     end
//     ;; the host function or the result conversion threw
//     trap
//   )
bool wasm::CreateLoweredFunction(
    JSContext* cx, Handle<WasmComponentInstanceObject*> instanceObj,
    const ComponentFuncType& funcType, const TypeDef& coreFuncType,
    HandleObject host, MutableHandleFunction result) {
  if (!CheckSupportedAdapterFuncType(cx, funcType)) {
    return false;
  }

  AdapterModuleBuilder builder(cx);
  uint32_t numParams = funcType.paramTypes.length();
  ValTypeVector hostParams, hostResults;
  if (!hostParams.appendN(ValType(RefType::extern_()), numParams) ||
      (funcType.resultType &&
       !hostResults.append(ValType(RefType::extern_())))) {
    ReportOutOfMemory(cx);
    return false;
  }
  uint32_t hostIndex;
  if (!builder.init(instanceObj) ||
      !builder.importJSFunc(host, std::move(hostParams), std::move(hostResults),
                            &hostIndex) ||
      !builder.defineAdapterFuncWithType(coreFuncType)) {
    return false;
  }

  const FuncType& coreType = coreFuncType.funcType();
  AdapterFuncEncoder f(builder.types(), coreType.args().length());

  // The flattened parameters are the adapter's own parameters.
  uint32_t nextFlatParam = 0;
  Vector<uint32_t, 8, SystemAllocPolicy> jsParams;
  for (const ComponentType& type : funcType.paramTypes) {
    ValTypeVector flatTypes;
    FlatLocals flat;
    if (!FlatTypes(type, &flatTypes)) {
      ReportOutOfMemory(cx);
      return false;
    }
    for (size_t i = 0; i < flatTypes.length(); i++) {
      if (!flat.append(nextFlatParam++)) {
        ReportOutOfMemory(cx);
        return false;
      }
    }
    uint32_t jsParam;
    if (!f.addLocal(ValType(RefType::extern_()), &jsParam) ||
        !jsParams.append(jsParam) || !EmitToJSValue(f, type, flat) ||
        !f.localSet(jsParam)) {
      ReportOutOfMemory(cx);
      return false;
    }
  }

  FlatLocals flatResult;
  mozilla::Maybe<ValType> flatResultType;
  uint32_t jsResult = 0;
  if (funcType.resultType &&
      (!AddFlatLocals(f, *funcType.resultType, &flatResult) ||
       !FlatResultType(*funcType.resultType, &flatResultType) ||
       !f.addLocal(ValType(RefType::extern_()), &jsResult))) {
    ReportOutOfMemory(cx);
    return false;
  }

  bool ok = f.block(Op::Block, mozilla::Nothing()) &&
            f.tryTableCatchAll(flatResultType, 0);
  for (uint32_t jsParam : jsParams) {
    ok = ok && f.localGet(jsParam);
  }
  ok = ok && f.call(hostIndex);
  if (funcType.resultType) {
    ok = ok && f.localSet(jsResult) &&
         EmitToComponentValue(f, *funcType.resultType, jsResult, flatResult) &&
         f.localGet(flatResult[0]);
  }
  ok = ok && f.op(Op::End) && f.op(Op::Return) && f.op(Op::End) &&
       f.trap(AdapterTrap::ImportException);

  Bytes body;
  if (!ok || !f.finish(&body)) {
    ReportOutOfMemory(cx);
    return false;
  }
  return builder.finish(body, result);
}

// ============================================================================
// Builtins

static ComponentInstance& InstanceFromCtx(HandleObject ctx) {
  return ctx->as<WasmComponentInstanceObject>().instance();
}

// The canonical ABI's lift_flat_signed and lift_flat_unsigned, which keep only
// the bits of the type's width.
static double LiftFlatInteger(int32_t value, ComponentTypeKind kind) {
  switch (kind) {
    case ComponentTypeKind::S8:
      return int8_t(value);
    case ComponentTypeKind::U8:
      return uint8_t(value);
    case ComponentTypeKind::S16:
      return int16_t(value);
    case ComponentTypeKind::U16:
      return uint16_t(value);
    case ComponentTypeKind::S32:
      return value;
    case ComponentTypeKind::U32:
      return uint32_t(value);
    default:
      MOZ_CRASH("not an integer type");
  }
}

// Returns a JS value as an externref for a builtin that fails with
// FailOnInvalidRef.
static void* ExternRefForCompiledCode(JSContext* cx, HandleValue value) {
  Rooted<AnyRef> result(cx);
  if (!AnyRef::fromJSValue(cx, value, &result)) {
    return AnyRef::invalid().forCompiledCode();
  }
  return result.get().forCompiledCode();
}

// Converts a JS value with one of the JS::ToInt8 family of conversions, for
// the sign- or zero-extended bits of the result.
template <typename T>
static bool ToIntegerBits(JSContext* cx, HandleValue value,
                          bool (*convert)(JSContext*, HandleValue, T*),
                          uint64_t* bits) {
  T n;
  if (!convert(cx, value, &n)) {
    return false;
  }
  *bits = uint64_t(n);
  return true;
}

// The JS-API's ToComponentValueInteger, for the bits of the integer.
static bool ToComponentValueInteger(JSContext* cx, HandleValue value,
                                    ComponentTypeKind kind, uint64_t* bits) {
  switch (kind) {
    case ComponentTypeKind::S8:
      return ToIntegerBits(cx, value, JS::ToInt8, bits);
    case ComponentTypeKind::U8:
      return ToIntegerBits(cx, value, JS::ToUint8, bits);
    case ComponentTypeKind::S16:
      return ToIntegerBits(cx, value, JS::ToInt16, bits);
    case ComponentTypeKind::U16:
      return ToIntegerBits(cx, value, JS::ToUint16, bits);
    case ComponentTypeKind::S32:
      return ToIntegerBits(cx, value, JS::ToInt32, bits);
    case ComponentTypeKind::U32:
      return ToIntegerBits(cx, value, JS::ToUint32, bits);
    case ComponentTypeKind::S64: {
      int64_t n;
      JS_TRY_VAR_OR_RETURN_FALSE(cx, n, ToBigInt64(cx, value));
      *bits = uint64_t(n);
      return true;
    }
    case ComponentTypeKind::U64:
      JS_TRY_VAR_OR_RETURN_FALSE(cx, *bits, ToBigUint64(cx, value));
      return true;
    default:
      MOZ_CRASH("not an integer type");
  }
}

int32_t wasm::CmToInteger(Instance* instance, void* ctxRef, void* valueRef,
                          int32_t kind) {
  MOZ_ASSERT(SASigCmToInteger.failureMode == FailureMode::FailOnNegI32);
  JSContext* cx = instance->cx();
  RootedObject ctx(cx, &AnyRef::fromCompiledCode(ctxRef).toJSObject());
  RootedValue value(cx, AnyRef::fromCompiledCode(valueRef).toJSValue());
  uint64_t bits;
  if (!ToComponentValueInteger(cx, value, ComponentTypeKind(kind), &bits)) {
    return -1;
  }
  InstanceFromCtx(ctx).setScratch(bits);
  return 0;
}

int32_t wasm::CmToFloat(Instance* instance, void* ctxRef, void* valueRef) {
  MOZ_ASSERT(SASigCmToFloat.failureMode == FailureMode::FailOnNegI32);
  JSContext* cx = instance->cx();
  RootedObject ctx(cx, &AnyRef::fromCompiledCode(ctxRef).toJSObject());
  RootedValue value(cx, AnyRef::fromCompiledCode(valueRef).toJSValue());
  double number;
  if (!ToNumber(cx, value, &number)) {
    return -1;
  }
  InstanceFromCtx(ctx).setScratch(mozilla::BitwiseCast<uint64_t>(number));
  return 0;
}

int32_t wasm::CmScratchI32(Instance* instance, void* ctxRef) {
  MOZ_ASSERT(SASigCmScratchI32.failureMode == FailureMode::Infallible);
  JSContext* cx = instance->cx();
  RootedObject ctx(cx, &AnyRef::fromCompiledCode(ctxRef).toJSObject());
  return int32_t(uint32_t(InstanceFromCtx(ctx).scratch()));
}

int64_t wasm::CmScratchI64(Instance* instance, void* ctxRef) {
  MOZ_ASSERT(SASigCmScratchI64.failureMode == FailureMode::Infallible);
  JSContext* cx = instance->cx();
  RootedObject ctx(cx, &AnyRef::fromCompiledCode(ctxRef).toJSObject());
  return int64_t(InstanceFromCtx(ctx).scratch());
}

double wasm::CmScratchF64(Instance* instance, void* ctxRef) {
  MOZ_ASSERT(SASigCmScratchF64.failureMode == FailureMode::Infallible);
  JSContext* cx = instance->cx();
  RootedObject ctx(cx, &AnyRef::fromCompiledCode(ctxRef).toJSObject());
  return mozilla::BitwiseCast<double>(InstanceFromCtx(ctx).scratch());
}

int32_t wasm::CmToBool(Instance* instance, void* valueRef) {
  MOZ_ASSERT(SASigCmToBool.failureMode == FailureMode::Infallible);
  JSContext* cx = instance->cx();
  RootedValue value(cx, AnyRef::fromCompiledCode(valueRef).toJSValue());
  return ToBoolean(value);
}

int32_t wasm::CmToChar(Instance* instance, void* valueRef) {
  MOZ_ASSERT(SASigCmToChar.failureMode == FailureMode::FailOnNegI32);
  JSContext* cx = instance->cx();
  RootedValue value(cx, AnyRef::fromCompiledCode(valueRef).toJSValue());
  JSString* string = ToString(cx, value);
  JSLinearString* str = string ? string->ensureLinear(cx) : nullptr;
  if (!str) {
    return -1;
  }

  // The string must be exactly one scalar value.
  char32_t codePoint;
  size_t length = str->length();
  if (length == 1 && !unicode::IsSurrogate(str->latin1OrTwoByteChar(0))) {
    codePoint = str->latin1OrTwoByteChar(0);
  } else if (length == 2 &&
             unicode::IsLeadSurrogate(str->latin1OrTwoByteChar(0)) &&
             unicode::IsTrailSurrogate(str->latin1OrTwoByteChar(1))) {
    codePoint = unicode::UTF16Decode(str->latin1OrTwoByteChar(0),
                                     str->latin1OrTwoByteChar(1));
  } else {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_WASM_COMPONENT_BAD_CHAR);
    return -1;
  }
  return int32_t(codePoint);
}

void* wasm::CmBoolToJS(Instance* instance, int32_t value) {
  MOZ_ASSERT(SASigCmBoolToJS.failureMode == FailureMode::FailOnInvalidRef);
  JSContext* cx = instance->cx();
  RootedValue jsValue(cx, BooleanValue(value != 0));
  return ExternRefForCompiledCode(cx, jsValue);
}

void* wasm::CmIntegerToJS(Instance* instance, int32_t value, int32_t kind) {
  MOZ_ASSERT(SASigCmIntegerToJS.failureMode == FailureMode::FailOnInvalidRef);
  JSContext* cx = instance->cx();
  RootedValue jsValue(
      cx, NumberValue(LiftFlatInteger(value, ComponentTypeKind(kind))));
  return ExternRefForCompiledCode(cx, jsValue);
}

void* wasm::CmInt64ToJS(Instance* instance, int64_t value, int32_t kind) {
  MOZ_ASSERT(SASigCmInt64ToJS.failureMode == FailureMode::FailOnInvalidRef);
  JSContext* cx = instance->cx();
  BigInt* bigInt = ComponentTypeKind(kind) == ComponentTypeKind::S64
                       ? BigInt::createFromInt64(cx, value)
                       : BigInt::createFromUint64(cx, uint64_t(value));
  if (!bigInt) {
    return AnyRef::invalid().forCompiledCode();
  }
  RootedValue jsValue(cx, BigIntValue(bigInt));
  return ExternRefForCompiledCode(cx, jsValue);
}

void* wasm::CmFloatToJS(Instance* instance, double value) {
  MOZ_ASSERT(SASigCmFloatToJS.failureMode == FailureMode::FailOnInvalidRef);
  JSContext* cx = instance->cx();
  RootedValue jsValue(cx, NumberValue(value));
  return ExternRefForCompiledCode(cx, jsValue);
}

void* wasm::CmCharToJS(Instance* instance, int32_t value) {
  MOZ_ASSERT(SASigCmCharToJS.failureMode == FailureMode::FailOnInvalidRef);
  JSContext* cx = instance->cx();
  uint32_t codePoint = uint32_t(value);
  if (codePoint > unicode::NonBMPMax || unicode::IsSurrogate(codePoint)) {
    ReportTrapError(cx, JSMSG_WASM_BAD_CODEPOINT);
    return AnyRef::invalid().forCompiledCode();
  }
  JSLinearString* str = StringFromCodePoint(cx, char32_t(codePoint));
  if (!str) {
    return AnyRef::invalid().forCompiledCode();
  }
  RootedValue jsValue(cx, StringValue(str));
  return ExternRefForCompiledCode(cx, jsValue);
}

int32_t wasm::CmTrap(Instance* instance, int32_t reason) {
  MOZ_ASSERT(SASigCmTrap.failureMode == FailureMode::FailOnNegI32);
  switch (AdapterTrap(reason)) {
    case AdapterTrap::UncaughtException:
      ReportTrapError(instance->cx(), JSMSG_WASM_COMPONENT_UNCAUGHT_EXCEPTION);
      break;
    case AdapterTrap::ImportException:
      ReportTrapError(instance->cx(), JSMSG_WASM_COMPONENT_IMPORT_EXCEPTION);
      break;
  }
  return -1;
}

#endif  // ENABLE_WASM_COMPONENTS
