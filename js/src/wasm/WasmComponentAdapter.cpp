/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "wasm/WasmComponentAdapter.h"

#ifdef ENABLE_WASM_COMPONENTS

#  include "js/CallAndConstruct.h"
#  include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#  include "vm/ErrorObject.h"
#  include "vm/JSFunction.h"
#  include "wasm/WasmBinary.h"
#  include "wasm/WasmBuiltinModule.h"
#  include "wasm/WasmComponentBuiltins.h"
#  include "wasm/WasmFeatures.h"
#  include "wasm/WasmGenerator.h"
#  include "wasm/WasmInstance.h"
#  include "wasm/WasmJS.h"
#  include "wasm/WasmValidate.h"

#  include "vm/JSObject-inl.h"
#  include "wasm/WasmInstance-inl.h"

using namespace js;
using namespace js::wasm;

// Every adapter module imports its WasmComponentInstanceObject as this global.
static constexpr uint32_t CtxGlobalIndex = 0;

namespace {

// The core wasm types a component value is flattened to, stored in locals.
using FlatLocals = Vector<uint32_t, 1, SystemAllocPolicy>;

// Builds and instantiates a module whose only defined function is an adapter,
// along with the imports it needs, and emits the adapter's code.
//
// Emitting an instruction can't fail. If memory runs out while emitting, the
// failure is reported by finish().
class MOZ_STACK_CLASS AdapterBuilder {
  JSContext* cx_;
  MutableModuleMetadata moduleMeta_;
  MutableCodeMetadata codeMeta_;
  Rooted<ImportValues> imports_;
  uint32_t adapterFuncIndex_ = UINT32_MAX;
  uint32_t numParams_ = 0;
  ValTypeVector locals_;
  Bytes code_;
  mozilla::Maybe<Encoder> encoder_;
  bool oom_ = false;

  Encoder& encoder() { return *encoder_; }
  void check(bool ok) {
    if (!ok) {
      oom_ = true;
    }
  }
  void varU32(uint32_t value) { check(encoder().writeVarU32(value)); }
  void fixedU8(uint8_t value) { check(encoder().writeFixedU8(value)); }

 protected:
  using Id = BuiltinModuleFuncId;

 public:
  explicit AdapterBuilder(JSContext* cx) : cx_(cx), imports_(cx) {}

  [[nodiscard]] bool init(Handle<WasmComponentInstanceObject*> instanceObj);

  // Imports a wasm function with its own type. Function imports must all be
  // added before the adapter function.
  [[nodiscard]] bool importWasmFunc(HandleFunction func, uint32_t* funcIndex);

  // Imports a function that is not a wasm function, which is called like a JS
  // function with this signature.
  [[nodiscard]] bool importJSFunc(HandleObject func, ValTypeVector&& params,
                                  ValTypeVector&& results, uint32_t* funcIndex);

  // Defines the adapter function, whose code is then emitted.
  [[nodiscard]] bool defineAdapterFunc(ValTypeVector&& params,
                                       ValTypeVector&& results);
  [[nodiscard]] bool defineAdapterFuncWithType(const TypeDef& funcType);

  // Compiles and instantiates the module, and gets its adapter function.
  [[nodiscard]] bool finish(MutableHandleFunction result);

  uint32_t addLocal(ValType type) {
    uint32_t index = numParams_ + locals_.length();
    check(locals_.append(type));
    return index;
  }
  // Adds a local for each of the flat types of `type`, appending them to
  // `locals`.
  [[nodiscard]] bool addFlatLocals(const ComponentType& type,
                                   FlatLocals* locals);

  void op(Op op) { check(encoder().writeOp(op)); }
  void end() { op(Op::End); }
  void localGet(uint32_t index) {
    op(Op::LocalGet);
    varU32(index);
  }
  void localSet(uint32_t index) {
    op(Op::LocalSet);
    varU32(index);
  }
  void globalGet(uint32_t index) {
    op(Op::GlobalGet);
    varU32(index);
  }
  // Pushes the WasmComponentInstanceObject.
  void ctx() { globalGet(CtxGlobalIndex); }
  void i32Const(int32_t value) {
    op(Op::I32Const);
    check(encoder().writeVarS32(value));
  }
  void call(uint32_t funcIndex) {
    op(Op::Call);
    varU32(funcIndex);
  }
  void callBuiltin(BuiltinModuleFuncId id) {
    check(encoder().writeOp(MozOp::CallBuiltinModuleFunc));
    varU32(uint32_t(id));
  }

  // Begins a block with no params and zero or one result.
  void block(Op op, mozilla::Maybe<ValType> result = mozilla::Nothing());
  // Begins a try_table that branches to `depth` on any exception.
  void tryTableCatchAll(uint32_t depth,
                        mozilla::Maybe<ValType> result = mozilla::Nothing());

  void trap(AdapterTrap reason);
};

}  // namespace

bool AdapterBuilder::init(Handle<WasmComponentInstanceObject*> instanceObj) {
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

bool AdapterBuilder::importWasmFunc(HandleFunction func, uint32_t* funcIndex) {
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

bool AdapterBuilder::importJSFunc(HandleObject func, ValTypeVector&& params,
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

bool AdapterBuilder::defineAdapterFuncWithType(const TypeDef& funcType) {
  MOZ_ASSERT(adapterFuncIndex_ == UINT32_MAX);
  MOZ_ASSERT(funcType.recGroup().numTypes() == 1);
  adapterFuncIndex_ = codeMeta_->funcs.length();
  numParams_ = funcType.funcType().args().length();
  encoder_.emplace(code_, *codeMeta_->types);
  uint32_t typeIndex = codeMeta_->types->length();
  if (!codeMeta_->types->addRecGroup(&funcType.recGroup()) ||
      !moduleMeta_->addDefinedFuncWithType(typeIndex, /*declareForRef=*/true,
                                           mozilla::Some(CacheableName()))) {
    ReportOutOfMemory(cx_);
    return false;
  }
  return true;
}

bool AdapterBuilder::defineAdapterFunc(ValTypeVector&& params,
                                       ValTypeVector&& results) {
  MOZ_ASSERT(adapterFuncIndex_ == UINT32_MAX);
  adapterFuncIndex_ = codeMeta_->funcs.length();
  numParams_ = params.length();
  encoder_.emplace(code_, *codeMeta_->types);
  if (!moduleMeta_->addDefinedFunc(std::move(params), std::move(results),
                                   /*declareForRef=*/true,
                                   mozilla::Some(CacheableName()))) {
    ReportOutOfMemory(cx_);
    return false;
  }
  return true;
}

bool AdapterBuilder::finish(MutableHandleFunction result) {
  MOZ_ASSERT(adapterFuncIndex_ != UINT32_MAX);

  Bytes body;
  Encoder bodyEncoder(body, *codeMeta_->types);
  if (oom_ || !EncodeLocalEntries(bodyEncoder, locals_) ||
      !body.appendAll(code_) || !bodyEncoder.writeOp(Op::End)) {
    ReportOutOfMemory(cx_);
    return false;
  }

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

[[nodiscard]] static bool FlatTypes(const ComponentType& type,
                                    ValTypeVector* flat) {
  bool hasStringsOrLists, tooDeep;
  return FlattenType(type, flat, &hasStringsOrLists, &tooDeep, 0);
}

bool AdapterBuilder::addFlatLocals(const ComponentType& type,
                                   FlatLocals* locals) {
  ValTypeVector flat;
  if (!FlatTypes(type, &flat)) {
    return false;
  }
  for (ValType flatType : flat) {
    if (!locals->append(addLocal(flatType))) {
      return false;
    }
  }
  return true;
}

void AdapterBuilder::block(Op op, mozilla::Maybe<ValType> result) {
  this->op(op);
  if (result) {
    check(encoder().writeValType(*result));
  } else {
    fixedU8(uint8_t(TypeCode::BlockVoid));
  }
}

void AdapterBuilder::tryTableCatchAll(uint32_t depth,
                                      mozilla::Maybe<ValType> result) {
  block(Op::TryTable, result);
  varU32(1);
  fixedU8(0x2);
  varU32(depth);
}

void AdapterBuilder::trap(AdapterTrap reason) {
  i32Const(int32_t(reason));
  callBuiltin(Id::ComponentTrap);
  op(Op::Unreachable);
}

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

namespace {

// Builds the adapter of a lifted or lowered function, which converts values
// between JS and the canonical ABI.
class MOZ_STACK_CLASS LiftLowerBuilder : public AdapterBuilder {
 public:
  explicit LiftLowerBuilder(JSContext* cx) : AdapterBuilder(cx) {}

  // Emits the body of the adapter for a lifted function, which calls
  // `calleeIndex`.
  [[nodiscard]] bool emitLiftAdapter(const ComponentFuncType& funcType,
                                     uint32_t calleeIndex);

  // Emits the body of the adapter for a lowered function, which calls
  // `hostIndex`.
  [[nodiscard]] bool emitLowerAdapter(const ComponentFuncType& funcType,
                                      uint32_t hostIndex);

 private:
  // Emits ToComponentValue for the JS value in local `jsValue`, storing the
  // flattened component value to `dest`.
  void emitToComponentValue(const ComponentType& type, uint32_t jsValue,
                            const FlatLocals& dest);
  // Emits ToJSValue for the flattened component value in `src`, leaving the JS
  // value on the stack.
  void emitToJSValue(const ComponentType& type, const FlatLocals& src);
};

}  // namespace

void LiftLowerBuilder::emitToComponentValue(const ComponentType& type,
                                            uint32_t jsValue,
                                            const FlatLocals& dest) {
  ComponentTypeKind kind = type.kind();
  localGet(jsValue);
  switch (kind) {
    case ComponentTypeKind::Bool:
      callBuiltin(Id::ComponentToBool);
      break;
    case ComponentTypeKind::S64:
    case ComponentTypeKind::U64:
      i32Const(int32_t(kind));
      callBuiltin(Id::ComponentToInteger);
      break;
    case ComponentTypeKind::F32:
    case ComponentTypeKind::F64:
      callBuiltin(Id::ComponentToFloat);
      if (kind == ComponentTypeKind::F32) {
        op(Op::F32DemoteF64);
      }
      break;
    case ComponentTypeKind::Char:
      callBuiltin(Id::ComponentToChar);
      break;
    default:
      MOZ_ASSERT(IsIntegerKind(kind));
      i32Const(int32_t(kind));
      callBuiltin(Id::ComponentToInteger);
      op(Op::I32WrapI64);
      break;
  }
  localSet(dest[0]);
}

void LiftLowerBuilder::emitToJSValue(const ComponentType& type,
                                     const FlatLocals& src) {
  ComponentTypeKind kind = type.kind();
  localGet(src[0]);
  switch (kind) {
    case ComponentTypeKind::Bool:
      callBuiltin(Id::ComponentBoolToJS);
      break;
    case ComponentTypeKind::S64:
    case ComponentTypeKind::U64:
      i32Const(int32_t(kind));
      callBuiltin(Id::ComponentInt64ToJS);
      break;
    case ComponentTypeKind::F32:
      op(Op::F64PromoteF32);
      callBuiltin(Id::ComponentFloatToJS);
      break;
    case ComponentTypeKind::F64:
      callBuiltin(Id::ComponentFloatToJS);
      break;
    case ComponentTypeKind::Char:
      callBuiltin(Id::ComponentCharToJS);
      break;
    default:
      MOZ_ASSERT(IsIntegerKind(kind));
      i32Const(int32_t(kind));
      callBuiltin(Id::ComponentIntegerToJS);
      break;
  }
}

// The adapter for a lifted function takes one externref for each parameter
// and returns an externref if the function has a result:
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
bool LiftLowerBuilder::emitLiftAdapter(const ComponentFuncType& funcType,
                                       uint32_t calleeIndex) {
  uint32_t numParams = funcType.paramTypes.length();
  Vector<FlatLocals, 8, SystemAllocPolicy> flatParams;
  if (!flatParams.resize(numParams)) {
    return false;
  }
  for (uint32_t i = 0; i < numParams; i++) {
    const ComponentType& type = funcType.paramTypes[i];
    if (!addFlatLocals(type, &flatParams[i])) {
      return false;
    }
    emitToComponentValue(type, i, flatParams[i]);
  }

  FlatLocals flatResult;
  mozilla::Maybe<ValType> flatResultType;
  if (funcType.resultType &&
      (!addFlatLocals(*funcType.resultType, &flatResult) ||
       !FlatResultType(*funcType.resultType, &flatResultType))) {
    return false;
  }

  block(Op::Block);
  tryTableCatchAll(0, flatResultType);
  for (const FlatLocals& locals : flatParams) {
    for (uint32_t local : locals) {
      localGet(local);
    }
  }
  call(calleeIndex);
  end();
  if (funcType.resultType) {
    localSet(flatResult[0]);
    emitToJSValue(*funcType.resultType, flatResult);
  }
  op(Op::Return);
  end();
  trap(AdapterTrap::UncaughtException);
  return true;
}

// The adapter for a lowered function has the flattened core function type and
// calls the host function with one externref for each parameter:
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
bool LiftLowerBuilder::emitLowerAdapter(const ComponentFuncType& funcType,
                                        uint32_t hostIndex) {
  // The flattened parameters are the adapter's own parameters.
  uint32_t nextFlatParam = 0;
  Vector<uint32_t, 8, SystemAllocPolicy> jsParams;
  for (const ComponentType& type : funcType.paramTypes) {
    ValTypeVector flatTypes;
    FlatLocals flat;
    if (!FlatTypes(type, &flatTypes)) {
      return false;
    }
    for (size_t i = 0; i < flatTypes.length(); i++) {
      if (!flat.append(nextFlatParam++)) {
        return false;
      }
    }
    uint32_t jsParam = addLocal(ValType(RefType::extern_()));
    if (!jsParams.append(jsParam)) {
      return false;
    }
    emitToJSValue(type, flat);
    localSet(jsParam);
  }

  FlatLocals flatResult;
  mozilla::Maybe<ValType> flatResultType;
  uint32_t jsResult = UINT32_MAX;
  if (funcType.resultType) {
    if (!addFlatLocals(*funcType.resultType, &flatResult) ||
        !FlatResultType(*funcType.resultType, &flatResultType)) {
      return false;
    }
    jsResult = addLocal(ValType(RefType::extern_()));
  }

  block(Op::Block);
  tryTableCatchAll(0, flatResultType);
  for (uint32_t jsParam : jsParams) {
    localGet(jsParam);
  }
  call(hostIndex);
  if (funcType.resultType) {
    localSet(jsResult);
    emitToComponentValue(*funcType.resultType, jsResult, flatResult);
    localGet(flatResult[0]);
  }
  end();
  op(Op::Return);
  end();
  trap(AdapterTrap::ImportException);
  return true;
}

// Generates the adapter for a lifted function.
[[nodiscard]] static bool GenerateLiftAdapter(
    JSContext* cx, Handle<WasmComponentInstanceObject*> instanceObj,
    const ComponentFuncType& funcType, HandleFunction callee,
    MutableHandleFunction result) {
  LiftLowerBuilder b(cx);
  uint32_t calleeIndex;
  if (!b.init(instanceObj) || !b.importWasmFunc(callee, &calleeIndex)) {
    return false;
  }

  ValTypeVector params, results;
  if (!params.appendN(ValType(RefType::extern_()),
                      funcType.paramTypes.length()) ||
      (funcType.resultType && !results.append(ValType(RefType::extern_())))) {
    ReportOutOfMemory(cx);
    return false;
  }
  if (!b.defineAdapterFunc(std::move(params), std::move(results))) {
    return false;
  }

  if (!b.emitLiftAdapter(funcType, calleeIndex)) {
    ReportOutOfMemory(cx);
    return false;
  }
  return b.finish(result);
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

bool wasm::CreateLoweredFunction(
    JSContext* cx, Handle<WasmComponentInstanceObject*> instanceObj,
    const ComponentFuncType& funcType, const TypeDef& coreFuncType,
    HandleObject host, MutableHandleFunction result) {
  if (!CheckSupportedAdapterFuncType(cx, funcType)) {
    return false;
  }

  ValTypeVector hostParams, hostResults;
  if (!hostParams.appendN(ValType(RefType::extern_()),
                          funcType.paramTypes.length()) ||
      (funcType.resultType &&
       !hostResults.append(ValType(RefType::extern_())))) {
    ReportOutOfMemory(cx);
    return false;
  }

  LiftLowerBuilder b(cx);
  uint32_t hostIndex;
  if (!b.init(instanceObj) ||
      !b.importJSFunc(host, std::move(hostParams), std::move(hostResults),
                      &hostIndex) ||
      !b.defineAdapterFuncWithType(coreFuncType)) {
    return false;
  }

  if (!b.emitLowerAdapter(funcType, hostIndex)) {
    ReportOutOfMemory(cx);
    return false;
  }
  return b.finish(result);
}

#endif  // ENABLE_WASM_COMPONENTS
