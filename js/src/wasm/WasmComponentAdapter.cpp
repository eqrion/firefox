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

// Where the core representation of a component value is: either flattened
// into locals, or stored in memory at a pointer (held in a local) plus an
// offset.
class ValueLoc {
  mozilla::Span<const uint32_t> flat_;
  uint32_t ptr_ = UINT32_MAX;
  uint32_t offset_ = 0;

 public:
  static ValueLoc flat(mozilla::Span<const uint32_t> locals) {
    ValueLoc loc;
    loc.flat_ = locals;
    return loc;
  }
  static ValueLoc memory(uint32_t ptrLocal, uint32_t offset) {
    ValueLoc loc;
    loc.ptr_ = ptrLocal;
    loc.offset_ = offset;
    return loc;
  }

  bool inMemory() const { return ptr_ != UINT32_MAX; }
  mozilla::Span<const uint32_t> flatLocals() const { return flat_; }
  uint32_t ptrLocal() const { return ptr_; }
  uint32_t offset() const { return offset_; }
};

using ValueLocVector = Vector<ValueLoc, 8, SystemAllocPolicy>;

// The core wasm types a component value is flattened to, stored in locals.
using FlatLocals = Vector<uint32_t, 2, SystemAllocPolicy>;

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

  JSContext* cx() const { return cx_; }

  // Imports a wasm function with its own type. Function imports must all be
  // added before the adapter function.
  [[nodiscard]] bool importWasmFunc(HandleFunction func, uint32_t* funcIndex);

  // Imports a function that is not a wasm function, which is called like a JS
  // function with this signature.
  [[nodiscard]] bool importJSFunc(HandleObject func, ValTypeVector&& params,
                                  ValTypeVector&& results, uint32_t* funcIndex);

  // Imports the memory for the canonical ABI as memory 0.
  [[nodiscard]] bool importMemory(Handle<WasmMemoryObject*> memory);

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
  // Adds a local for each of `flatTypes`, appending them to `locals`.
  [[nodiscard]] bool addFlatLocals(const ValTypeVector& flatTypes,
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
  void refNullExtern() {
    op(Op::RefNull);
    fixedU8(uint8_t(TypeCode::ExternRef));
  }
  void zero(ValType type);
  void call(uint32_t funcIndex) {
    op(Op::Call);
    varU32(funcIndex);
  }
  void callBuiltin(BuiltinModuleFuncId id) {
    check(encoder().writeOp(MozOp::CallBuiltinModuleFunc));
    varU32(uint32_t(id));
  }

  // Loads and stores from memory 0.
  void memoryAccess(Op op, uint32_t alignLog2, uint32_t offset) {
    this->op(op);
    varU32(alignLog2);
    varU32(offset);
  }

  // Begins a block with no params and zero or one result.
  void block(Op op, mozilla::Maybe<ValType> result = mozilla::Nothing());
  // Begins a try_table that branches to `depth` on any exception.
  void tryTableCatchAll(uint32_t depth,
                        mozilla::Maybe<ValType> result = mozilla::Nothing());
  // Begins a try_table with no result that branches to `depth` with the
  // exnref on any exception.
  void tryTableCatchAllRef(uint32_t depth);
  void br(uint32_t depth) {
    op(Op::Br);
    varU32(depth);
  }
  void brIf(uint32_t depth) {
    op(Op::BrIf);
    varU32(depth);
  }
  // Branches to depth `i` for `i` in [0, count), and to depth `count`
  // otherwise.
  void brTableCases(uint32_t count);

  void trap(AdapterTrap reason);

  // Emits `emitBody`, trapping for `reason` if it throws.
  template <typename F>
  [[nodiscard]] bool trapping(AdapterTrap reason, F emitBody);

  // Calls a function of the guest, trapping if it throws. `emitArgs` emits
  // the arguments.
  template <typename F>
  void callGuest(uint32_t funcIndex, mozilla::Maybe<ValType> result,
                 F emitArgs);

  // Pushes a JS value, which is imported as an immutable externref global. A
  // value is only imported once.
  [[nodiscard]] bool pushValue(HandleValue value);

  // Pushes a scalar from `loc`.
  void emitGetScalar(ComponentTypeKind kind, const ValueLoc& loc);
  // Stores the scalar on the stack to `loc`.
  void emitSetScalar(ComponentTypeKind kind, const ValueLoc& loc);
  // Pushes the i32 at `index` of a value made of i32s, like a string.
  void emitGetI32(const ValueLoc& loc, uint32_t index);
  // Stores the i32 in local `value` at `index` of a value made of i32s.
  void emitSetI32(const ValueLoc& loc, uint32_t index, uint32_t value);

  // Pushes the discriminant of the variant at `loc`.
  void emitGetDiscriminant(size_t numCases, const ValueLoc& loc);
  // Stores the discriminant in local `disc` to the variant at `loc`.
  void emitSetDiscriminant(size_t numCases, const ValueLoc& loc, uint32_t disc);
  // The canonical ABI's coercions between a case's flat type and the joined
  // flat type of all the cases, when lifting and lowering a flattened variant.
  void emitCoerceFromJoined(ValType joined, ValType want);
  void emitCoerceToJoined(ValType have, ValType joined);

  // Emits a branch on the case index pushed by `emitIndex` to the code of each
  // case of a variant, with `emitCase(i, depth)` emitting the code for case
  // `i`, which branches to `depth` to finish. `emitDefault` emits the code for
  // an index that is not a case.
  template <typename E, typename F, typename G>
  [[nodiscard]] bool emitCaseSwitch(size_t numCases, E emitIndex, F emitCase,
                                    G emitDefault);

  // Emits a loop over `i` from 0 to `count`, where `elemPtr` is `ptr` plus
  // `i * elemSize`, with `emitBody` emitting the body.
  template <typename F>
  [[nodiscard]] bool emitListLoop(uint32_t ptr, uint32_t count,
                                  uint32_t elemSize, uint32_t i,
                                  uint32_t elemPtr, F emitBody);

  // Traps unless `ptr` is aligned for `type` and all of `type` fits in memory.
  void emitCheckPointer(const ComponentType& type, uint32_t ptr);
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

bool AdapterBuilder::importMemory(Handle<WasmMemoryObject*> memory) {
  MOZ_ASSERT(codeMeta_->memories.empty());
  MOZ_ASSERT(memory->addressType() == AddressType::I32 && !memory->isShared());
  Limits limits(/*initial=*/0, mozilla::Nothing(), Shareable::False,
                memory->pageSize());
  uint32_t importIndex = moduleMeta_->imports.length();
  if (!codeMeta_->memories.emplaceBack(MemoryDesc(limits)) ||
      !moduleMeta_->imports.emplaceBack(CacheableName(), CacheableName(),
                                        DefinitionKind::Memory) ||
      !imports_.get().memories.append(memory)) {
    ReportOutOfMemory(cx_);
    return false;
  }
  codeMeta_->memories.back().importIndex = mozilla::Some(importIndex);
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

bool AdapterBuilder::addFlatLocals(const ValTypeVector& flatTypes,
                                   FlatLocals* locals) {
  for (ValType flatType : flatTypes) {
    if (!locals->append(addLocal(flatType))) {
      return false;
    }
  }
  return true;
}

void AdapterBuilder::zero(ValType type) {
  switch (type.kind()) {
    case ValType::I32:
      i32Const(0);
      return;
    case ValType::I64:
      op(Op::I64Const);
      check(encoder().writeVarS64(0));
      return;
    case ValType::F32:
      op(Op::F32Const);
      check(encoder().writeFixedF32(0));
      return;
    case ValType::F64:
      op(Op::F64Const);
      check(encoder().writeFixedF64(0));
      return;
    default:
      MOZ_CRASH("not a flat type");
  }
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

void AdapterBuilder::tryTableCatchAllRef(uint32_t depth) {
  block(Op::TryTable);
  varU32(1);
  fixedU8(0x3);
  varU32(depth);
}

void AdapterBuilder::brTableCases(uint32_t count) {
  op(Op::BrTable);
  varU32(count);
  for (uint32_t i = 0; i < count; i++) {
    varU32(i);
  }
  varU32(count);
}

void AdapterBuilder::trap(AdapterTrap reason) {
  i32Const(int32_t(reason));
  callBuiltin(Id::ComponentTrap);
  op(Op::Unreachable);
}

template <typename F>
bool AdapterBuilder::trapping(AdapterTrap reason, F emitBody) {
  block(Op::Block);
  block(Op::Block);
  tryTableCatchAll(0);
  if (!emitBody()) {
    return false;
  }
  end();
  br(1);
  end();
  trap(reason);
  end();
  return true;
}

template <typename F>
void AdapterBuilder::callGuest(uint32_t funcIndex,
                               mozilla::Maybe<ValType> result, F emitArgs) {
  block(Op::Block, result);
  block(Op::Block);
  tryTableCatchAll(0, result);
  emitArgs();
  call(funcIndex);
  end();
  br(1);
  end();
  trap(AdapterTrap::UncaughtException);
  end();
}

bool AdapterBuilder::pushValue(HandleValue value) {
  ValVector& values = imports_.get().globalValues;
  for (size_t i = 0; i < values.length(); i++) {
    if (values[i].ref().toJSValue() == value) {
      globalGet(i);
      return true;
    }
  }

  Rooted<AnyRef> ref(cx_);
  if (!AnyRef::fromJSValue(cx_, value, &ref)) {
    return false;
  }
  uint32_t globalIndex = codeMeta_->globals.length();
  GlobalType type(ValType(RefType::extern_()), /*isMutable=*/false);
  if (!codeMeta_->globals.append(GlobalDesc(type, globalIndex)) ||
      !moduleMeta_->imports.emplaceBack(CacheableName(), CacheableName(),
                                        DefinitionKind::Global) ||
      !values.append(Val(ValType(RefType::extern_()), ref.get()))) {
    ReportOutOfMemory(cx_);
    return false;
  }
  globalGet(globalIndex);
  return true;
}

// How a scalar is loaded from and stored to memory, and its flat type.
struct ScalarLayout {
  Op load;
  Op store;
  uint32_t alignLog2;
  ValType flatType;
};

static ScalarLayout GetScalarLayout(ComponentTypeKind kind) {
  switch (kind) {
    case ComponentTypeKind::Bool:
    case ComponentTypeKind::U8:
      return {Op::I32Load8U, Op::I32Store8, 0, ValType::I32};
    case ComponentTypeKind::S8:
      return {Op::I32Load8S, Op::I32Store8, 0, ValType::I32};
    case ComponentTypeKind::U16:
      return {Op::I32Load16U, Op::I32Store16, 1, ValType::I32};
    case ComponentTypeKind::S16:
      return {Op::I32Load16S, Op::I32Store16, 1, ValType::I32};
    case ComponentTypeKind::S32:
    case ComponentTypeKind::U32:
    case ComponentTypeKind::Char:
      return {Op::I32Load, Op::I32Store, 2, ValType::I32};
    case ComponentTypeKind::S64:
    case ComponentTypeKind::U64:
      return {Op::I64Load, Op::I64Store, 3, ValType::I64};
    case ComponentTypeKind::F32:
      return {Op::F32Load, Op::F32Store, 2, ValType::F32};
    case ComponentTypeKind::F64:
      return {Op::F64Load, Op::F64Store, 3, ValType::F64};
    default:
      MOZ_CRASH("not a scalar type");
  }
}

void AdapterBuilder::emitGetScalar(ComponentTypeKind kind,
                                   const ValueLoc& loc) {
  if (!loc.inMemory()) {
    localGet(loc.flatLocals()[0]);
    return;
  }
  ScalarLayout layout = GetScalarLayout(kind);
  localGet(loc.ptrLocal());
  memoryAccess(layout.load, layout.alignLog2, loc.offset());
}

void AdapterBuilder::emitSetScalar(ComponentTypeKind kind,
                                   const ValueLoc& loc) {
  if (!loc.inMemory()) {
    localSet(loc.flatLocals()[0]);
    return;
  }
  ScalarLayout layout = GetScalarLayout(kind);
  uint32_t value = addLocal(layout.flatType);
  localSet(value);
  localGet(loc.ptrLocal());
  localGet(value);
  memoryAccess(layout.store, layout.alignLog2, loc.offset());
}

void AdapterBuilder::emitGetI32(const ValueLoc& loc, uint32_t index) {
  if (!loc.inMemory()) {
    localGet(loc.flatLocals()[index]);
    return;
  }
  localGet(loc.ptrLocal());
  memoryAccess(Op::I32Load, 2, loc.offset() + 4 * index);
}

void AdapterBuilder::emitSetI32(const ValueLoc& loc, uint32_t index,
                                uint32_t value) {
  if (!loc.inMemory()) {
    localGet(value);
    localSet(loc.flatLocals()[index]);
    return;
  }
  localGet(loc.ptrLocal());
  localGet(value);
  memoryAccess(Op::I32Store, 2, loc.offset() + 4 * index);
}

void AdapterBuilder::emitGetDiscriminant(size_t numCases, const ValueLoc& loc) {
  if (!loc.inMemory()) {
    localGet(loc.flatLocals()[0]);
    return;
  }
  localGet(loc.ptrLocal());
  switch (CanonDiscriminantSize(numCases)) {
    case 1:
      memoryAccess(Op::I32Load8U, 0, loc.offset());
      return;
    case 2:
      memoryAccess(Op::I32Load16U, 1, loc.offset());
      return;
    default:
      memoryAccess(Op::I32Load, 2, loc.offset());
      return;
  }
}

void AdapterBuilder::emitSetDiscriminant(size_t numCases, const ValueLoc& loc,
                                         uint32_t disc) {
  if (!loc.inMemory()) {
    localGet(disc);
    localSet(loc.flatLocals()[0]);
    return;
  }
  localGet(loc.ptrLocal());
  localGet(disc);
  switch (CanonDiscriminantSize(numCases)) {
    case 1:
      memoryAccess(Op::I32Store8, 0, loc.offset());
      return;
    case 2:
      memoryAccess(Op::I32Store16, 1, loc.offset());
      return;
    default:
      memoryAccess(Op::I32Store, 2, loc.offset());
      return;
  }
}

void AdapterBuilder::emitCoerceFromJoined(ValType joined, ValType want) {
  if (joined == want) {
    return;
  }
  if (joined == ValType::I32) {
    MOZ_ASSERT(want == ValType::F32);
    op(Op::F32ReinterpretI32);
    return;
  }
  MOZ_ASSERT(joined == ValType::I64);
  switch (want.kind()) {
    case ValType::I32:
      op(Op::I32WrapI64);
      return;
    case ValType::F32:
      op(Op::I32WrapI64);
      op(Op::F32ReinterpretI32);
      return;
    case ValType::F64:
      op(Op::F64ReinterpretI64);
      return;
    default:
      MOZ_CRASH("unexpected flat type");
  }
}

void AdapterBuilder::emitCoerceToJoined(ValType have, ValType joined) {
  if (have == joined) {
    return;
  }
  if (joined == ValType::I32) {
    MOZ_ASSERT(have == ValType::F32);
    op(Op::I32ReinterpretF32);
    return;
  }
  MOZ_ASSERT(joined == ValType::I64);
  switch (have.kind()) {
    case ValType::I32:
      op(Op::I64ExtendI32U);
      return;
    case ValType::F32:
      op(Op::I32ReinterpretF32);
      op(Op::I64ExtendI32U);
      return;
    case ValType::F64:
      op(Op::I64ReinterpretF64);
      return;
    default:
      MOZ_CRASH("unexpected flat type");
  }
}

template <typename E, typename F, typename G>
bool AdapterBuilder::emitCaseSwitch(size_t numCases, E emitIndex, F emitCase,
                                    G emitDefault) {
  //   block $done
  //     block $default
  //       (block $case)*
  //         br_table $case* $default
  //       end
  //       ;; case i
  //       br $done
  //       ...
  //     end
  //     ;; default
  //   end
  block(Op::Block);
  block(Op::Block);
  for (size_t i = 0; i < numCases; i++) {
    block(Op::Block);
  }
  emitIndex();
  brTableCases(numCases);
  for (size_t i = 0; i < numCases; i++) {
    end();
    if (!emitCase(i, uint32_t(numCases - i))) {
      return false;
    }
  }
  end();
  emitDefault();
  end();
  return true;
}

template <typename F>
bool AdapterBuilder::emitListLoop(uint32_t ptr, uint32_t count,
                                  uint32_t elemSize, uint32_t i,
                                  uint32_t elemPtr, F emitBody) {
  i32Const(0);
  localSet(i);
  localGet(ptr);
  localSet(elemPtr);
  block(Op::Block);
  block(Op::Loop);
  localGet(i);
  localGet(count);
  op(Op::I32GeU);
  brIf(1);
  if (!emitBody()) {
    return false;
  }
  localGet(elemPtr);
  i32Const(int32_t(elemSize));
  op(Op::I32Add);
  localSet(elemPtr);
  localGet(i);
  i32Const(1);
  op(Op::I32Add);
  localSet(i);
  br(0);
  end();
  end();
  return true;
}

void AdapterBuilder::emitCheckPointer(const ComponentType& type, uint32_t ptr) {
  localGet(ptr);
  i32Const(1);
  i32Const(int32_t(CanonElemSize(type)));
  i32Const(int32_t(CanonAlignment(type)));
  callBuiltin(Id::ComponentCheckRange);
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

[[nodiscard]] static bool IsSupportedVariant(const ComponentType& type);

static bool IsSupportedAdapterType(const ComponentType& type) {
  switch (type.kind()) {
    case ComponentTypeKind::Variant:
    case ComponentTypeKind::Enum:
    case ComponentTypeKind::Option:
    case ComponentTypeKind::Result:
      return IsSupportedVariant(type);
    case ComponentTypeKind::Bool:
    case ComponentTypeKind::S64:
    case ComponentTypeKind::U64:
    case ComponentTypeKind::F32:
    case ComponentTypeKind::F64:
    case ComponentTypeKind::Char:
    case ComponentTypeKind::String:
    case ComponentTypeKind::Flags:
      return true;
    case ComponentTypeKind::List:
      return IsSupportedAdapterType(type.asList());
    case ComponentTypeKind::Tuple:
      for (const ComponentType& item : type.asTuple()) {
        if (!IsSupportedAdapterType(item)) {
          return false;
        }
      }
      return true;
    case ComponentTypeKind::Record:
      for (const ComponentRecordField& field : type.asRecord()) {
        if (!IsSupportedAdapterType(field.type)) {
          return false;
        }
      }
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

[[nodiscard]] static bool FlatTypes(const ComponentType& type,
                                    ValTypeVector* flat) {
  bool hasStringsOrLists, tooDeep;
  return FlattenType(type, flat, &hasStringsOrLists, &tooDeep, 0);
}

// Computes the location of each field of a tuple or record at `loc`.
[[nodiscard]] static bool GetFieldLocs(const ComponentTypeVector& fieldTypes,
                                       const ValueLoc& loc,
                                       ValueLocVector* fieldLocs) {
  uint32_t offset = 0;
  size_t flatIndex = 0;
  for (const ComponentType& fieldType : fieldTypes) {
    if (loc.inMemory()) {
      offset = CanonAlignTo(offset, CanonAlignment(fieldType));
      if (!fieldLocs->append(
              ValueLoc::memory(loc.ptrLocal(), loc.offset() + offset))) {
        return false;
      }
      offset += CanonElemSize(fieldType);
    } else {
      ValTypeVector flatTypes;
      if (!FlatTypes(fieldType, &flatTypes) ||
          !fieldLocs->append(ValueLoc::flat(
              loc.flatLocals().Subspan(flatIndex, flatTypes.length())))) {
        return false;
      }
      flatIndex += flatTypes.length();
    }
  }
  return true;
}

[[nodiscard]] static bool GetRecordFieldTypes(const ComponentType& type,
                                              ComponentTypeVector* types) {
  for (const ComponentRecordField& field : type.asRecord()) {
    if (!types->append(field.type)) {
      return false;
    }
  }
  return true;
}

// How flags are stored in memory, which is by their number of labels.
static ScalarLayout GetFlagsLayout(const ComponentType& type) {
  switch (CanonElemSize(type)) {
    case 1:
      return {Op::I32Load8U, Op::I32Store8, 0, ValType::I32};
    case 2:
      return {Op::I32Load16U, Op::I32Store16, 1, ValType::I32};
    default:
      return {Op::I32Load, Op::I32Store, 2, ValType::I32};
  }
}

// How a variant-like type (variant, enum, option, or result) is represented in
// JS.
enum class VariantRepr {
  // An object with a `kind`, and a `value` if the case has a payload. This is
  // for variants, results outside of return position, and options that are the
  // payload of an option.
  Object,
  // undefined, or the payload. This is for other options.
  Optional,
  // The case's label. This is for enums.
  Label,
};

// The representation of a variant-like type, which is `inOption` if it is the
// payload of an option.
static VariantRepr GetVariantRepr(const ComponentType& type, bool inOption) {
  switch (type.kind()) {
    case ComponentTypeKind::Enum:
      return VariantRepr::Label;
    case ComponentTypeKind::Option:
      return inOption ? VariantRepr::Object : VariantRepr::Optional;
    default:
      return VariantRepr::Object;
  }
}

namespace {

struct VariantCase {
  mozilla::Span<const char> label;
  mozilla::Maybe<ComponentType> type;
};
using VariantCaseVector = Vector<VariantCase, 4, SystemAllocPolicy>;

}  // namespace

[[nodiscard]] static bool GetVariantCases(const ComponentType& type,
                                          VariantCaseVector* cases) {
  switch (type.kind()) {
    case ComponentTypeKind::Variant:
      for (const ComponentVariantCase& c : type.asVariant()) {
        if (!cases->append(VariantCase{c.name.utf8Bytes(), c.type})) {
          return false;
        }
      }
      return true;
    case ComponentTypeKind::Enum:
      for (const CacheableName& label : type.asEnum()) {
        if (!cases->append(
                VariantCase{label.utf8Bytes(), mozilla::Nothing()})) {
          return false;
        }
      }
      return true;
    case ComponentTypeKind::Option:
      return cases->append(VariantCase{mozilla::MakeStringSpan("none"),
                                       mozilla::Nothing()}) &&
             cases->append(VariantCase{mozilla::MakeStringSpan("some"),
                                       mozilla::Some(type.asOption())});
    case ComponentTypeKind::Result: {
      ComponentResultType result = type.asResult();
      return cases->append(
                 VariantCase{mozilla::MakeStringSpan("ok"), result.type}) &&
             cases->append(VariantCase{mozilla::MakeStringSpan("error"),
                                       result.errorType});
    }
    default:
      MOZ_CRASH("not a variant-like type");
  }
}

[[nodiscard]] static bool IsSupportedVariant(const ComponentType& type) {
  VariantCaseVector cases;
  if (!GetVariantCases(type, &cases)) {
    return false;
  }
  for (const VariantCase& c : cases) {
    if (c.type && !IsSupportedAdapterType(*c.type)) {
      return false;
    }
  }
  return true;
}

// The location of the payload of the variant at `loc`.
static ValueLoc GetPayloadLoc(const ComponentType& type, const ValueLoc& loc) {
  if (loc.inMemory()) {
    return ValueLoc::memory(loc.ptrLocal(),
                            loc.offset() + CanonVariantPayloadOffset(type));
  }
  return ValueLoc::flat(loc.flatLocals().From(1));
}

// Gets the flat types of the payloads of all the cases of a variant.
[[nodiscard]] static bool GetJoinedTypes(const ComponentType& type,
                                         ValTypeVector* joinedTypes) {
  ValTypeVector flatTypes;
  return FlatTypes(type, &flatTypes) &&
         joinedTypes->append(flatTypes.begin() + 1, flatTypes.end());
}
namespace {

// Builds the adapter of a lifted or lowered function, which converts values
// between JS and the canonical ABI with the function's canonical options.
class MOZ_STACK_CLASS LiftLowerBuilder : public AdapterBuilder {
  // Whether calls to realloc enter the guest, which is true when lifting.
  bool enterGuest_;
  ComponentStringEncoding stringEncoding_ = ComponentStringEncoding::UTF8;
  // The realloc function, or UINT32_MAX if there is none.
  uint32_t realloc_ = UINT32_MAX;

 public:
  LiftLowerBuilder(JSContext* cx, bool enterGuest)
      : AdapterBuilder(cx), enterGuest_(enterGuest) {}

  // Imports the memory and functions of the canonical options, and sets up
  // `postReturn` with its function index, or UINT32_MAX if it is absent.
  [[nodiscard]] bool importCanonOpts(const CanonOptValues& opts,
                                     uint32_t* postReturn);

  // Emits the body of the adapter for a lifted function, which calls
  // `calleeIndex`.
  [[nodiscard]] bool emitLiftAdapter(const ComponentFuncType& funcType,
                                     uint32_t calleeIndex, uint32_t postReturn);

  // Emits the body of the adapter for a lowered function, which calls
  // `hostIndex`, and has the core function type `coreType`.
  [[nodiscard]] bool emitLowerAdapter(const ComponentFuncType& funcType,
                                      const FuncType& coreType,
                                      uint32_t hostIndex);

 private:
  // Emits ToComponentValue for the JS value in local `jsValue`, storing the
  // component value to `loc`.
  [[nodiscard]] bool emitToComponentValue(const ComponentType& type,
                                          uint32_t jsValue,
                                          const ValueLoc& loc);
  // Emits ToJSValue for the component value at `loc`, leaving the JS value on
  // the stack.
  [[nodiscard]] bool emitToJSValue(const ComponentType& type,
                                   const ValueLoc& loc);

  // Calls realloc, with `emitOld` emitting its first two arguments, and stores
  // the result in local `ptr`.
  template <typename F>
  void emitRealloc(F emitOld, int32_t alignment, uint32_t newSize,
                   uint32_t ptr);
  // The canonical ABI's allocate, of the size in local `size`.
  void emitAllocate(int32_t alignment, uint32_t size, uint32_t ptr);
  // The canonical ABI's reallocate, of `ptr` from `oldSize` to `newSize`, all
  // locals.
  void emitReallocate(uint32_t oldSize, int32_t alignment, uint32_t newSize,
                      uint32_t ptr);

  // Pushes the JS name of a label.
  [[nodiscard]] bool emitName(mozilla::Span<const char> label,
                              ComponentJSCase jsCase);
  // Pushes an array of the JS names of the cases, for builtins to match.
  [[nodiscard]] bool emitCaseNames(const VariantCaseVector& cases,
                                   ComponentJSCase jsCase);

  void emitStoreString(uint32_t jsValue, const ValueLoc& loc);

  [[nodiscard]] bool emitToComponentValueList(const ComponentType& type,
                                              uint32_t jsValue,
                                              const ValueLoc& loc);
  [[nodiscard]] bool emitToJSValueList(const ComponentType& type,
                                       const ValueLoc& loc);
  [[nodiscard]] bool emitToComponentValueTuple(const ComponentType& type,
                                               uint32_t jsValue,
                                               const ValueLoc& loc);
  [[nodiscard]] bool emitToJSValueTuple(const ComponentType& type,
                                        const ValueLoc& loc);
  [[nodiscard]] bool emitToComponentValueRecord(const ComponentType& type,
                                                uint32_t jsValue,
                                                const ValueLoc& loc);
  [[nodiscard]] bool emitToJSValueRecord(const ComponentType& type,
                                         const ValueLoc& loc);
  [[nodiscard]] bool emitToComponentValueFlags(const ComponentType& type,
                                               uint32_t jsValue,
                                               const ValueLoc& loc);
  [[nodiscard]] bool emitToJSValueFlags(const ComponentType& type,
                                        const ValueLoc& loc);

  // ToComponentValue and ToJSValue for a variant-like type, which is
  // `inOption` if it is the payload of an option.
  [[nodiscard]] bool emitToComponentValueVariant(const ComponentType& type,
                                                 uint32_t jsValue,
                                                 const ValueLoc& loc,
                                                 bool inOption);
  [[nodiscard]] bool emitToJSValueVariant(const ComponentType& type,
                                          const ValueLoc& loc, bool inOption);
  // ToComponentValue and ToJSValue for the payload of a case, which is
  // `inOption` if the variant is an option.
  [[nodiscard]] bool emitCaseToComponentValue(const ComponentType& caseType,
                                              uint32_t jsValue,
                                              const ValueLoc& loc,
                                              bool inOption);
  [[nodiscard]] bool emitCaseToJSValue(const ComponentType& caseType,
                                       const ValueLoc& loc, bool inOption);
  // ToComponentValue for the payload of a case, if any, to `payloadLoc`. The
  // flat values of the payloads that the case doesn't use are zeroed.
  [[nodiscard]] bool emitPayloadToComponentValue(
      const mozilla::Maybe<ComponentType>& caseType, uint32_t jsValue,
      const ValueLoc& payloadLoc, const ValTypeVector& joinedTypes,
      bool inOption);
  // ToJSValue for the payload of a case at `payloadLoc`, where `joinedTypes`
  // are the flat types of the payloads of all the cases, and `inOption` is
  // whether the variant is an option.
  [[nodiscard]] bool emitPayloadToJSValue(const ComponentType& caseType,
                                          const ValueLoc& payloadLoc,
                                          const ValTypeVector& joinedTypes,
                                          bool inOption);

  // ToJSValue for a lifted function's result, into local `jsResult`. A
  // `result` is unwrapped: the payload of `ok` is the JS result, and the
  // payload of `error` becomes the data of a ComponentError that is thrown
  // once the function returns.
  [[nodiscard]] bool emitLiftedResultToJSValue(const ComponentType& type,
                                               const ValueLoc& loc,
                                               uint32_t jsResult);

  // Calls the host function of a lowered function that returns a `result`: a
  // normal return is the `ok` case, with the returned value as its payload,
  // and a thrown exception is the `error` case, with the exception as its
  // payload. A payload that fails to convert traps. `emitArgs` emits the host
  // function's arguments, and `emitReturn` returns from the adapter.
  template <typename F, typename G>
  [[nodiscard]] bool emitLoweredResultCall(const ComponentType& type,
                                           uint32_t hostIndex,
                                           const ValueLoc& loc,
                                           mozilla::Maybe<uint32_t> retPtr,
                                           F emitArgs, G emitReturn);
};

}  // namespace

bool LiftLowerBuilder::importCanonOpts(const CanonOptValues& opts,
                                       uint32_t* postReturn) {
  stringEncoding_ = opts.stringEncoding;
  *postReturn = UINT32_MAX;
  return (!opts.memory || importMemory(opts.memory)) &&
         (!opts.realloc || importWasmFunc(opts.realloc, &realloc_)) &&
         (!opts.postReturn || importWasmFunc(opts.postReturn, postReturn));
}

template <typename F>
void LiftLowerBuilder::emitRealloc(F emitOld, int32_t alignment,
                                   uint32_t newSize, uint32_t ptr) {
  MOZ_ASSERT(realloc_ != UINT32_MAX);
  if (enterGuest_) {
    ctx();
    callBuiltin(Id::ComponentEnterGuest);
  }
  callGuest(realloc_, mozilla::Some(ValType::I32), [&]() {
    emitOld();
    i32Const(alignment);
    localGet(newSize);
  });
  localSet(ptr);
}

void LiftLowerBuilder::emitAllocate(int32_t alignment, uint32_t size,
                                    uint32_t ptr) {
  emitRealloc(
      [&]() {
        i32Const(0);
        i32Const(0);
      },
      alignment, size, ptr);
}

void LiftLowerBuilder::emitReallocate(uint32_t oldSize, int32_t alignment,
                                      uint32_t newSize, uint32_t ptr) {
  emitRealloc(
      [&]() {
        localGet(ptr);
        localGet(oldSize);
      },
      alignment, newSize, ptr);
}

bool LiftLowerBuilder::emitName(mozilla::Span<const char> label,
                                ComponentJSCase jsCase) {
  JSAtom* atom = ComponentLabelToJS(cx(), label, jsCase);
  if (!atom) {
    return false;
  }
  RootedValue name(cx(), StringValue(atom));
  return pushValue(name);
}

bool LiftLowerBuilder::emitCaseNames(const VariantCaseVector& cases,
                                     ComponentJSCase jsCase) {
  Rooted<ArrayObject*> names(cx(), NewDenseEmptyArray(cx()));
  if (!names) {
    return false;
  }
  for (const VariantCase& c : cases) {
    JSAtom* name = ComponentLabelToJS(cx(), c.label, jsCase);
    if (!name || !NewbornArrayPush(cx(), names, StringValue(name))) {
      return false;
    }
  }
  RootedValue namesValue(cx(), ObjectValue(*names));
  return pushValue(namesValue);
}

// ToComponentValue for a string, with the canonical ABI's store_string. JS
// strings are stored as if their encoding is UTF-16.
void LiftLowerBuilder::emitStoreString(uint32_t jsValue, const ValueLoc& loc) {
  uint32_t str = addLocal(ValType(RefType::extern_()));
  uint32_t length = addLocal(ValType::I32);
  uint32_t ptr = addLocal(ValType::I32);
  uint32_t byteLength = addLocal(ValType::I32);
  localGet(jsValue);
  callBuiltin(Id::ComponentToString);
  localSet(str);
  localGet(str);
  callBuiltin(Id::ComponentStringLength);
  localSet(length);

  switch (stringEncoding_) {
    case ComponentStringEncoding::UTF8: {
      // Optimistically allocate one byte per code unit, and if the string
      // turns out not to be ASCII, grow to the worst case and then shrink.
      uint32_t asciiLength = addLocal(ValType::I32);
      emitAllocate(1, length, ptr);
      localGet(str);
      localGet(ptr);
      localGet(length);
      callBuiltin(Id::ComponentStoreASCII);
      localSet(asciiLength);
      localGet(asciiLength);
      localGet(length);
      op(Op::I32Ne);
      block(Op::If);
      {
        uint32_t worstCase = addLocal(ValType::I32);
        localGet(length);
        i32Const(3);
        op(Op::I32Mul);
        localSet(worstCase);
        emitReallocate(length, 1, worstCase, ptr);
        localGet(str);
        localGet(ptr);
        localGet(worstCase);
        callBuiltin(Id::ComponentStoreUTF8);
        localSet(byteLength);
        localGet(worstCase);
        localGet(byteLength);
        op(Op::I32GtU);
        block(Op::If);
        {
          emitReallocate(worstCase, 1, byteLength, ptr);
          localGet(ptr);
          localGet(byteLength);
          i32Const(1);
          i32Const(1);
          callBuiltin(Id::ComponentCheckRange);
        }
        end();
        localGet(byteLength);
        localSet(length);
      }
      end();
      break;
    }
    case ComponentStringEncoding::UTF16:
      localGet(length);
      i32Const(1);
      op(Op::I32Shl);
      localSet(byteLength);
      emitAllocate(2, byteLength, ptr);
      localGet(str);
      localGet(ptr);
      localGet(length);
      callBuiltin(Id::ComponentStoreUTF16);
      break;
    case ComponentStringEncoding::Latin1PlusUTF16: {
      // This is store_string_to_latin1_or_utf16. Because the source is UTF-16,
      // storing it as UTF-16 takes exactly two bytes per code unit, and so
      // neither of the reallocs that shrink the allocation happen.
      uint32_t latin1Length = addLocal(ValType::I32);
      emitAllocate(2, length, ptr);
      localGet(str);
      localGet(ptr);
      localGet(length);
      callBuiltin(Id::ComponentStoreLatin1);
      localSet(latin1Length);
      localGet(latin1Length);
      localGet(length);
      op(Op::I32Ne);
      block(Op::If);
      {
        localGet(length);
        i32Const(1);
        op(Op::I32Shl);
        localSet(byteLength);
        emitReallocate(length, 2, byteLength, ptr);
        localGet(str);
        localGet(ptr);
        localGet(latin1Length);
        callBuiltin(Id::ComponentStoreLatin1ToUTF16);
        localGet(length);
        i32Const(int32_t(UTF16Tag));
        op(Op::I32Or);
        localSet(length);
      }
      end();
      break;
    }
  }

  emitSetI32(loc, 0, ptr);
  emitSetI32(loc, 1, length);
}

// ToComponentValue for a tuple: as for a list, which must have exactly as many
// elements as the tuple.
bool LiftLowerBuilder::emitToComponentValueTuple(const ComponentType& type,
                                                 uint32_t jsValue,
                                                 const ValueLoc& loc) {
  const ComponentTypeVector& itemTypes = type.asTuple();
  ValueLocVector itemLocs;
  if (!GetFieldLocs(itemTypes, loc, &itemLocs)) {
    return false;
  }
  uint32_t array = addLocal(ValType(RefType::extern_()));
  localGet(jsValue);
  i32Const(0);
  callBuiltin(Id::ComponentIterableToArray);
  localSet(array);
  localGet(array);
  i32Const(int32_t(itemTypes.length()));
  callBuiltin(Id::ComponentCheckArrayLength);
  for (size_t i = 0; i < itemTypes.length(); i++) {
    uint32_t item = addLocal(ValType(RefType::extern_()));
    localGet(array);
    i32Const(int32_t(i));
    callBuiltin(Id::ComponentArrayGet);
    localSet(item);
    if (!emitToComponentValue(itemTypes[i], item, itemLocs[i])) {
      return false;
    }
  }
  return true;
}

bool LiftLowerBuilder::emitToJSValueTuple(const ComponentType& type,
                                          const ValueLoc& loc) {
  const ComponentTypeVector& itemTypes = type.asTuple();
  ValueLocVector itemLocs;
  if (!GetFieldLocs(itemTypes, loc, &itemLocs)) {
    return false;
  }
  uint32_t array = addLocal(ValType(RefType::extern_()));
  i32Const(int32_t(itemTypes.length()));
  callBuiltin(Id::ComponentNewArray);
  localSet(array);
  for (size_t i = 0; i < itemTypes.length(); i++) {
    localGet(array);
    i32Const(int32_t(i));
    if (!emitToJSValue(itemTypes[i], itemLocs[i])) {
      return false;
    }
    callBuiltin(Id::ComponentArraySet);
  }
  localGet(array);
  return true;
}

// The JS-API's ToComponentValueRecord.
bool LiftLowerBuilder::emitToComponentValueRecord(const ComponentType& type,
                                                  uint32_t jsValue,
                                                  const ValueLoc& loc) {
  const ComponentRecordFieldVector& fields = type.asRecord();
  ComponentTypeVector fieldTypes;
  ValueLocVector fieldLocs;
  if (!GetRecordFieldTypes(type, &fieldTypes) ||
      !GetFieldLocs(fieldTypes, loc, &fieldLocs)) {
    return false;
  }
  localGet(jsValue);
  callBuiltin(Id::ComponentCheckObject);
  for (size_t i = 0; i < fields.length(); i++) {
    uint32_t field = addLocal(ValType(RefType::extern_()));
    localGet(jsValue);
    if (!emitName(fields[i].name.utf8Bytes(), ComponentJSCase::Camel)) {
      return false;
    }
    callBuiltin(Id::ComponentGetField);
    localSet(field);
    if (!emitToComponentValue(fields[i].type, field, fieldLocs[i])) {
      return false;
    }
  }
  return true;
}

// The JS-API's ToJSValueRecord.
bool LiftLowerBuilder::emitToJSValueRecord(const ComponentType& type,
                                           const ValueLoc& loc) {
  const ComponentRecordFieldVector& fields = type.asRecord();
  ComponentTypeVector fieldTypes;
  ValueLocVector fieldLocs;
  if (!GetRecordFieldTypes(type, &fieldTypes) ||
      !GetFieldLocs(fieldTypes, loc, &fieldLocs)) {
    return false;
  }
  uint32_t record = addLocal(ValType(RefType::extern_()));
  callBuiltin(Id::ComponentNewRecord);
  localSet(record);
  for (size_t i = 0; i < fields.length(); i++) {
    localGet(record);
    if (!emitName(fields[i].name.utf8Bytes(), ComponentJSCase::Camel) ||
        !emitToJSValue(fields[i].type, fieldLocs[i])) {
      return false;
    }
    callBuiltin(Id::ComponentDefineField);
  }
  localGet(record);
  return true;
}

// The JS-API's ToComponentValueFlags.
bool LiftLowerBuilder::emitToComponentValueFlags(const ComponentType& type,
                                                 uint32_t jsValue,
                                                 const ValueLoc& loc) {
  const CacheableNameVector& labels = type.asFlags();
  uint32_t bits = addLocal(ValType::I32);
  localGet(jsValue);
  callBuiltin(Id::ComponentCheckObject);
  for (size_t i = 0; i < labels.length(); i++) {
    // A missing label is false.
    localGet(bits);
    localGet(jsValue);
    if (!emitName(labels[i].utf8Bytes(), ComponentJSCase::Camel)) {
      return false;
    }
    callBuiltin(Id::ComponentGetField);
    callBuiltin(Id::ComponentToBool);
    i32Const(int32_t(i));
    op(Op::I32Shl);
    op(Op::I32Or);
    localSet(bits);
  }
  if (!loc.inMemory()) {
    localGet(bits);
    localSet(loc.flatLocals()[0]);
    return true;
  }
  ScalarLayout layout = GetFlagsLayout(type);
  localGet(loc.ptrLocal());
  localGet(bits);
  memoryAccess(layout.store, layout.alignLog2, loc.offset());
  return true;
}

// The JS-API's ToJSValueFlags.
bool LiftLowerBuilder::emitToJSValueFlags(const ComponentType& type,
                                          const ValueLoc& loc) {
  const CacheableNameVector& labels = type.asFlags();
  uint32_t bits = addLocal(ValType::I32);
  uint32_t flags = addLocal(ValType(RefType::extern_()));
  if (loc.inMemory()) {
    ScalarLayout layout = GetFlagsLayout(type);
    localGet(loc.ptrLocal());
    memoryAccess(layout.load, layout.alignLog2, loc.offset());
  } else {
    localGet(loc.flatLocals()[0]);
  }
  localSet(bits);
  callBuiltin(Id::ComponentNewRecord);
  localSet(flags);
  for (size_t i = 0; i < labels.length(); i++) {
    localGet(flags);
    if (!emitName(labels[i].utf8Bytes(), ComponentJSCase::Camel)) {
      return false;
    }
    localGet(bits);
    i32Const(int32_t(i));
    op(Op::I32ShrU);
    i32Const(1);
    op(Op::I32And);
    callBuiltin(Id::ComponentBoolToJS);
    callBuiltin(Id::ComponentDefineField);
  }
  localGet(flags);
  return true;
}

bool LiftLowerBuilder::emitCaseToJSValue(const ComponentType& caseType,
                                         const ValueLoc& loc, bool inOption) {
  if (inOption && caseType.kind() == ComponentTypeKind::Option) {
    return emitToJSValueVariant(caseType, loc, /*inOption=*/true);
  }
  return emitToJSValue(caseType, loc);
}

bool LiftLowerBuilder::emitCaseToComponentValue(const ComponentType& caseType,
                                                uint32_t jsValue,
                                                const ValueLoc& loc,
                                                bool inOption) {
  if (inOption && caseType.kind() == ComponentTypeKind::Option) {
    return emitToComponentValueVariant(caseType, jsValue, loc,
                                       /*inOption=*/true);
  }
  return emitToComponentValue(caseType, jsValue, loc);
}

bool LiftLowerBuilder::emitPayloadToJSValue(const ComponentType& caseType,
                                            const ValueLoc& payloadLoc,
                                            const ValTypeVector& joinedTypes,
                                            bool inOption) {
  if (payloadLoc.inMemory()) {
    return emitCaseToJSValue(caseType, payloadLoc, inOption);
  }

  ValTypeVector caseTypes;
  FlatLocals caseLocals;
  if (!FlatTypes(caseType, &caseTypes) ||
      !addFlatLocals(caseTypes, &caseLocals)) {
    return false;
  }
  for (size_t i = 0; i < caseTypes.length(); i++) {
    localGet(payloadLoc.flatLocals()[i]);
    emitCoerceFromJoined(joinedTypes[i], caseTypes[i]);
    localSet(caseLocals[i]);
  }
  return emitCaseToJSValue(caseType, ValueLoc::flat(caseLocals), inOption);
}

bool LiftLowerBuilder::emitPayloadToComponentValue(
    const mozilla::Maybe<ComponentType>& caseType, uint32_t jsValue,
    const ValueLoc& payloadLoc, const ValTypeVector& joinedTypes,
    bool inOption) {
  if (payloadLoc.inMemory()) {
    return !caseType ||
           emitCaseToComponentValue(*caseType, jsValue, payloadLoc, inOption);
  }

  ValTypeVector caseTypes;
  if (caseType) {
    FlatLocals caseLocals;
    if (!FlatTypes(*caseType, &caseTypes) ||
        !addFlatLocals(caseTypes, &caseLocals) ||
        !emitCaseToComponentValue(*caseType, jsValue,
                                  ValueLoc::flat(caseLocals), inOption)) {
      return false;
    }
    for (size_t i = 0; i < caseTypes.length(); i++) {
      localGet(caseLocals[i]);
      emitCoerceToJoined(caseTypes[i], joinedTypes[i]);
      localSet(payloadLoc.flatLocals()[i]);
    }
  }
  for (size_t i = caseTypes.length(); i < joinedTypes.length(); i++) {
    zero(joinedTypes[i]);
    localSet(payloadLoc.flatLocals()[i]);
  }
  return true;
}

bool LiftLowerBuilder::emitToJSValueVariant(const ComponentType& type,
                                            const ValueLoc& loc,
                                            bool inOption) {
  VariantCaseVector cases;
  ValTypeVector joinedTypes;
  if (!GetVariantCases(type, &cases) ||
      (!loc.inMemory() && !GetJoinedTypes(type, &joinedTypes))) {
    return false;
  }
  uint32_t result = addLocal(ValType(RefType::extern_()));
  VariantRepr repr = GetVariantRepr(type, inOption);
  bool isOption = type.kind() == ComponentTypeKind::Option;
  ValueLoc payloadLoc = GetPayloadLoc(type, loc);
  auto emitCase = [&](size_t i, uint32_t depth) {
    const VariantCase& c = cases[i];
    switch (repr) {
      case VariantRepr::Object:
        if (!emitName(c.label, ComponentJSCase::Verbatim)) {
          return false;
        }
        if (c.type) {
          if (!emitPayloadToJSValue(*c.type, payloadLoc, joinedTypes,
                                    isOption)) {
            return false;
          }
          i32Const(1);
        } else {
          refNullExtern();
          i32Const(0);
        }
        callBuiltin(Id::ComponentNewVariant);
        break;
      case VariantRepr::Optional: {
        bool ok = c.type ? emitPayloadToJSValue(*c.type, payloadLoc,
                                                joinedTypes, isOption)
                         : pushValue(UndefinedHandleValue);
        if (!ok) {
          return false;
        }
        break;
      }
      case VariantRepr::Label:
        if (!emitName(c.label, ComponentJSCase::Verbatim)) {
          return false;
        }
        break;
    }
    localSet(result);
    br(depth);
    return true;
  };
  auto emitIndex = [&]() { emitGetDiscriminant(cases.length(), loc); };
  auto emitDefault = [&]() { trap(AdapterTrap::BadDiscriminant); };
  if (!emitCaseSwitch(cases.length(), emitIndex, emitCase, emitDefault)) {
    return false;
  }
  localGet(result);
  return true;
}

bool LiftLowerBuilder::emitToComponentValueVariant(const ComponentType& type,
                                                   uint32_t jsValue,
                                                   const ValueLoc& loc,
                                                   bool inOption) {
  VariantCaseVector cases;
  ValTypeVector joinedTypes;
  if (!GetVariantCases(type, &cases) ||
      (!loc.inMemory() && !GetJoinedTypes(type, &joinedTypes))) {
    return false;
  }
  uint32_t disc = addLocal(ValType::I32);

  VariantRepr repr = GetVariantRepr(type, inOption);
  localGet(jsValue);
  switch (repr) {
    case VariantRepr::Object:
      if (!emitCaseNames(cases, ComponentJSCase::Verbatim)) {
        return false;
      }
      callBuiltin(Id::ComponentFindVariantCase);
      break;
    case VariantRepr::Optional:
      callBuiltin(Id::ComponentIsNullish);
      op(Op::I32Eqz);
      break;
    case VariantRepr::Label:
      if (!emitCaseNames(cases, ComponentJSCase::Verbatim)) {
        return false;
      }
      callBuiltin(Id::ComponentFindEnumCase);
      break;
  }
  localSet(disc);
  emitSetDiscriminant(cases.length(), loc, disc);

  ValueLoc payloadLoc = GetPayloadLoc(type, loc);
  auto emitCase = [&](size_t i, uint32_t depth) {
    const VariantCase& c = cases[i];
    uint32_t payload = jsValue;
    if (c.type && repr == VariantRepr::Object) {
      payload = addLocal(ValType(RefType::extern_()));
      localGet(jsValue);
      if (!emitName(mozilla::MakeStringSpan("value"),
                    ComponentJSCase::Verbatim)) {
        return false;
      }
      callBuiltin(Id::ComponentGetField);
      localSet(payload);
    }
    if (!emitPayloadToComponentValue(
            c.type, payload, payloadLoc, joinedTypes,
            type.kind() == ComponentTypeKind::Option)) {
      return false;
    }
    br(depth);
    return true;
  };
  auto emitIndex = [&]() { localGet(disc); };
  // The discriminant is always the index of a case.
  auto emitDefault = [&]() { op(Op::Unreachable); };
  return emitCaseSwitch(cases.length(), emitIndex, emitCase, emitDefault);
}

// ToComponentValue for a list, with the canonical ABI's store_list.
bool LiftLowerBuilder::emitToComponentValueList(const ComponentType& type,
                                                uint32_t jsValue,
                                                const ValueLoc& loc) {
  ComponentType elemType = type.asList();
  uint32_t elemSize = CanonElemSize(elemType);
  uint32_t alignment = CanonAlignment(elemType);
  uint32_t ptr = addLocal(ValType::I32);
  uint32_t length = addLocal(ValType::I32);
  uint32_t byteLength = addLocal(ValType::I32);
  block(Op::Block);

  // A typed array of the list's scalar type is copied directly.
  int32_t kind = int32_t(elemType.kind());
  if (ScalarListArrayType(elemType.kind())) {
    localGet(jsValue);
    i32Const(kind);
    callBuiltin(Id::ComponentIsScalarArray);
    block(Op::If);
    {
      localGet(jsValue);
      i32Const(kind);
      callBuiltin(Id::ComponentScalarArrayLength);
      localSet(length);
      localGet(length);
      i32Const(int32_t(elemSize));
      op(Op::I32Mul);
      localSet(byteLength);
      emitAllocate(int32_t(alignment), byteLength, ptr);
      localGet(jsValue);
      localGet(ptr);
      localGet(length);
      callBuiltin(Id::ComponentCopyScalars);
      br(1);
    }
    end();
  }

  uint32_t array = addLocal(ValType(RefType::extern_()));
  uint32_t i = addLocal(ValType::I32);
  uint32_t elemPtr = addLocal(ValType::I32);
  uint32_t elem = addLocal(ValType(RefType::extern_()));
  localGet(jsValue);
  i32Const(int32_t(elemSize));
  callBuiltin(Id::ComponentIterableToArray);
  localSet(array);
  localGet(array);
  callBuiltin(Id::ComponentArrayLength);
  localSet(length);
  localGet(length);
  i32Const(int32_t(elemSize));
  op(Op::I32Mul);
  localSet(byteLength);
  emitAllocate(int32_t(alignment), byteLength, ptr);
  localGet(ptr);
  localGet(length);
  i32Const(int32_t(elemSize));
  i32Const(int32_t(alignment));
  callBuiltin(Id::ComponentCheckRange);
  if (!emitListLoop(ptr, length, elemSize, i, elemPtr, [&]() {
        localGet(array);
        localGet(i);
        callBuiltin(Id::ComponentArrayGet);
        localSet(elem);
        return emitToComponentValue(elemType, elem,
                                    ValueLoc::memory(elemPtr, 0));
      })) {
    return false;
  }
  end();

  emitSetI32(loc, 0, ptr);
  emitSetI32(loc, 1, length);
  return true;
}

// ToJSValue for a list, with the canonical ABI's load_list.
bool LiftLowerBuilder::emitToJSValueList(const ComponentType& type,
                                         const ValueLoc& loc) {
  ComponentType elemType = type.asList();

  // A list of scalars becomes a typed array.
  if (ScalarListArrayType(elemType.kind())) {
    emitGetI32(loc, 0);
    emitGetI32(loc, 1);
    i32Const(int32_t(elemType.kind()));
    callBuiltin(Id::ComponentScalarListToJS);
    return true;
  }

  uint32_t elemSize = CanonElemSize(elemType);
  uint32_t ptr = addLocal(ValType::I32);
  uint32_t length = addLocal(ValType::I32);
  uint32_t array = addLocal(ValType(RefType::extern_()));
  uint32_t i = addLocal(ValType::I32);
  uint32_t elemPtr = addLocal(ValType::I32);
  emitGetI32(loc, 0);
  localSet(ptr);
  emitGetI32(loc, 1);
  localSet(length);
  localGet(ptr);
  localGet(length);
  i32Const(int32_t(elemSize));
  i32Const(int32_t(CanonAlignment(elemType)));
  callBuiltin(Id::ComponentCheckRange);
  localGet(length);
  callBuiltin(Id::ComponentNewArray);
  localSet(array);
  if (!emitListLoop(ptr, length, elemSize, i, elemPtr, [&]() {
        localGet(array);
        localGet(i);
        if (!emitToJSValue(elemType, ValueLoc::memory(elemPtr, 0))) {
          return false;
        }
        callBuiltin(Id::ComponentArraySet);
        return true;
      })) {
    return false;
  }
  localGet(array);
  return true;
}

bool LiftLowerBuilder::emitToComponentValue(const ComponentType& type,
                                            uint32_t jsValue,
                                            const ValueLoc& loc) {
  ComponentTypeKind kind = type.kind();
  switch (kind) {
    case ComponentTypeKind::Bool:
      localGet(jsValue);
      callBuiltin(Id::ComponentToBool);
      emitSetScalar(kind, loc);
      return true;
    case ComponentTypeKind::S64:
    case ComponentTypeKind::U64:
      localGet(jsValue);
      i32Const(int32_t(kind));
      callBuiltin(Id::ComponentToInteger);
      emitSetScalar(kind, loc);
      return true;
    case ComponentTypeKind::F32:
    case ComponentTypeKind::F64:
      localGet(jsValue);
      callBuiltin(Id::ComponentToFloat);
      if (kind == ComponentTypeKind::F32) {
        op(Op::F32DemoteF64);
      }
      emitSetScalar(kind, loc);
      return true;
    case ComponentTypeKind::Char:
      localGet(jsValue);
      callBuiltin(Id::ComponentToChar);
      emitSetScalar(kind, loc);
      return true;
    case ComponentTypeKind::String:
      emitStoreString(jsValue, loc);
      return true;
    case ComponentTypeKind::List:
      return emitToComponentValueList(type, jsValue, loc);
    case ComponentTypeKind::Tuple:
      return emitToComponentValueTuple(type, jsValue, loc);
    case ComponentTypeKind::Record:
      return emitToComponentValueRecord(type, jsValue, loc);
    case ComponentTypeKind::Flags:
      return emitToComponentValueFlags(type, jsValue, loc);
    case ComponentTypeKind::Variant:
    case ComponentTypeKind::Enum:
    case ComponentTypeKind::Option:
    case ComponentTypeKind::Result:
      return emitToComponentValueVariant(type, jsValue, loc,
                                         /*inOption=*/false);
    default:
      MOZ_ASSERT(IsIntegerKind(kind));
      localGet(jsValue);
      i32Const(int32_t(kind));
      callBuiltin(Id::ComponentToInteger);
      op(Op::I32WrapI64);
      emitSetScalar(kind, loc);
      return true;
  }
}

bool LiftLowerBuilder::emitToJSValue(const ComponentType& type,
                                     const ValueLoc& loc) {
  ComponentTypeKind kind = type.kind();
  switch (kind) {
    case ComponentTypeKind::Bool:
      emitGetScalar(kind, loc);
      callBuiltin(Id::ComponentBoolToJS);
      return true;
    case ComponentTypeKind::S64:
    case ComponentTypeKind::U64:
      emitGetScalar(kind, loc);
      i32Const(int32_t(kind));
      callBuiltin(Id::ComponentInt64ToJS);
      return true;
    case ComponentTypeKind::F32:
      emitGetScalar(kind, loc);
      op(Op::F64PromoteF32);
      callBuiltin(Id::ComponentFloatToJS);
      return true;
    case ComponentTypeKind::F64:
      emitGetScalar(kind, loc);
      callBuiltin(Id::ComponentFloatToJS);
      return true;
    case ComponentTypeKind::Char:
      emitGetScalar(kind, loc);
      callBuiltin(Id::ComponentCharToJS);
      return true;
    case ComponentTypeKind::String:
      emitGetI32(loc, 0);
      emitGetI32(loc, 1);
      i32Const(int32_t(stringEncoding_));
      callBuiltin(Id::ComponentStringToJS);
      return true;
    case ComponentTypeKind::List:
      return emitToJSValueList(type, loc);
    case ComponentTypeKind::Tuple:
      return emitToJSValueTuple(type, loc);
    case ComponentTypeKind::Record:
      return emitToJSValueRecord(type, loc);
    case ComponentTypeKind::Flags:
      return emitToJSValueFlags(type, loc);
    case ComponentTypeKind::Variant:
    case ComponentTypeKind::Enum:
    case ComponentTypeKind::Option:
    case ComponentTypeKind::Result:
      return emitToJSValueVariant(type, loc, /*inOption=*/false);
    default:
      MOZ_ASSERT(IsIntegerKind(kind));
      emitGetScalar(kind, loc);
      i32Const(int32_t(kind));
      callBuiltin(Id::ComponentIntegerToJS);
      return true;
  }
}

bool LiftLowerBuilder::emitLiftedResultToJSValue(const ComponentType& type,
                                                 const ValueLoc& loc,
                                                 uint32_t jsResult) {
  if (type.kind() != ComponentTypeKind::Result) {
    if (!emitToJSValue(type, loc)) {
      return false;
    }
    localSet(jsResult);
    return true;
  }

  ComponentResultType resultType = type.asResult();
  ValTypeVector joinedTypes;
  if (!loc.inMemory() && !GetJoinedTypes(type, &joinedTypes)) {
    return false;
  }
  ValueLoc payloadLoc = GetPayloadLoc(type, loc);
  auto emitIndex = [&]() { emitGetDiscriminant(2, loc); };
  auto emitCase = [&](size_t i, uint32_t depth) {
    if (i == 0) {
      bool ok = resultType.type
                    ? emitPayloadToJSValue(*resultType.type, payloadLoc,
                                           joinedTypes, /*inOption=*/false)
                    : pushValue(UndefinedHandleValue);
      if (!ok) {
        return false;
      }
    } else {
      ctx();
      if (resultType.errorType) {
        if (!emitPayloadToJSValue(*resultType.errorType, payloadLoc,
                                  joinedTypes, /*inOption=*/false)) {
          return false;
        }
        i32Const(1);
      } else {
        refNullExtern();
        i32Const(0);
      }
      callBuiltin(Id::ComponentSetError);
      if (!pushValue(UndefinedHandleValue)) {
        return false;
      }
    }
    localSet(jsResult);
    br(depth);
    return true;
  };
  auto emitDefault = [&]() { trap(AdapterTrap::BadDiscriminant); };
  return emitCaseSwitch(2, emitIndex, emitCase, emitDefault);
}

template <typename F, typename G>
bool LiftLowerBuilder::emitLoweredResultCall(const ComponentType& type,
                                             uint32_t hostIndex,
                                             const ValueLoc& loc,
                                             mozilla::Maybe<uint32_t> retPtr,
                                             F emitArgs, G emitReturn) {
  ComponentResultType resultType = type.asResult();
  ValTypeVector joinedTypes;
  if (!loc.inMemory() && !GetJoinedTypes(type, &joinedTypes)) {
    return false;
  }
  uint32_t jsResult = addLocal(ValType(RefType::extern_()));
  uint32_t exn = addLocal(ValType(RefType::exn()));
  uint32_t disc = addLocal(ValType::I32);
  ValueLoc payloadLoc = GetPayloadLoc(type, loc);

  auto emitStoreCase = [&](int32_t index) {
    const mozilla::Maybe<ComponentType>& caseType =
        index == 0 ? resultType.type : resultType.errorType;
    uint32_t payload = jsResult;
    if (index == 1 && caseType) {
      payload = addLocal(ValType(RefType::extern_()));
      localGet(exn);
      callBuiltin(Id::ComponentExceptionValue);
      localSet(payload);
    }
    if (retPtr) {
      emitCheckPointer(type, *retPtr);
    }
    i32Const(index);
    localSet(disc);
    emitSetDiscriminant(2, loc, disc);
    return emitPayloadToComponentValue(caseType, payload, payloadLoc,
                                       joinedTypes, /*inOption=*/false);
  };

  //   block $ok
  //     block $caught (result exnref)
  //       try_table (catch_all_ref $caught)
  //         call $host
  //         br $ok
  //       end
  //       unreachable
  //     end
  //     ;; store the error case, trapping if it throws
  //     return
  //   end
  //   ;; store the ok case, trapping if it throws
  block(Op::Block);
  block(Op::Block, mozilla::Some(ValType(RefType::exn())));
  tryTableCatchAllRef(0);
  emitArgs();
  call(hostIndex);
  localSet(jsResult);
  br(2);
  end();
  op(Op::Unreachable);
  end();
  localSet(exn);
  if (!trapping(AdapterTrap::ImportException,
                [&]() { return emitStoreCase(1); })) {
    return false;
  }
  emitReturn();
  end();
  return trapping(AdapterTrap::ImportException,
                  [&]() { return emitStoreCase(0); });
}

// The adapter for a lifted function takes one externref for each parameter
// and returns an externref if the function has a result:
//
//   (func (param externref)* (result externref)?
//     ;; ToComponentValue for each parameter, into flat locals
//     ;; call $callee, trapping if it throws
//     ;; ToJSValue for the result, from flat locals or the result pointer
//     ;; call $post-return, if any
//   )
bool LiftLowerBuilder::emitLiftAdapter(const ComponentFuncType& funcType,
                                       uint32_t calleeIndex,
                                       uint32_t postReturn) {
  uint32_t numParams = funcType.paramTypes.length();
  FlatLocals flatArgs;
  for (uint32_t i = 0; i < numParams; i++) {
    const ComponentType& type = funcType.paramTypes[i];
    ValTypeVector flatTypes;
    size_t start = flatArgs.length();
    if (!FlatTypes(type, &flatTypes) || !addFlatLocals(flatTypes, &flatArgs) ||
        !emitToComponentValue(
            type, i,
            ValueLoc::flat(
                mozilla::Span<const uint32_t>(flatArgs).From(start)))) {
      return false;
    }
  }

  // Results with more than one flat value are returned through a pointer.
  ValTypeVector flatResultTypes;
  if (funcType.resultType &&
      !FlatTypes(*funcType.resultType, &flatResultTypes)) {
    return false;
  }
  bool useRetPtr = flatResultTypes.length() > MaxFlatResults;
  mozilla::Maybe<ValType> calleeResultType;
  uint32_t calleeResult = UINT32_MAX;
  if (!flatResultTypes.empty()) {
    calleeResultType.emplace(useRetPtr ? ValType::I32 : flatResultTypes[0]);
    calleeResult = addLocal(*calleeResultType);
  }

  ctx();
  callBuiltin(Id::ComponentEnterGuest);
  callGuest(calleeIndex, calleeResultType, [&]() {
    for (uint32_t local : flatArgs) {
      localGet(local);
    }
  });
  if (calleeResultType) {
    localSet(calleeResult);
  }

  uint32_t jsResult = UINT32_MAX;
  if (funcType.resultType) {
    ValueLoc loc = useRetPtr ? ValueLoc::memory(calleeResult, 0)
                             : ValueLoc::flat(mozilla::Span(&calleeResult, 1));
    jsResult = addLocal(ValType(RefType::extern_()));
    if (useRetPtr) {
      emitCheckPointer(*funcType.resultType, calleeResult);
    }
    if (!emitLiftedResultToJSValue(*funcType.resultType, loc, jsResult)) {
      return false;
    }
  }

  if (postReturn != UINT32_MAX) {
    callGuest(postReturn, mozilla::Nothing(), [&]() {
      if (calleeResultType) {
        localGet(calleeResult);
      }
    });
  }
  if (funcType.resultType) {
    localGet(jsResult);
  }
  return true;
}

// The adapter for a lowered function has the flattened core function type and
// calls the host function with one externref for each parameter:
//
//   (func (param flat-param)* (param $retptr i32)? (result flat-result)?
//     ;; ToJSValue for each parameter, into externref locals
//     block
//       try_table (result flat-result)? (catch_all 0)
//         (local.get js-param)*
//         call $host
//         ;; ToComponentValue for the result, if any, into a flat local or
//         ;; to $retptr
//       end
//       return
//     end
//     ;; the host function or the result conversion threw
//     trap
//   )
bool LiftLowerBuilder::emitLowerAdapter(const ComponentFuncType& funcType,
                                        const FuncType& coreType,
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
    if (!jsParams.append(jsParam) ||
        !emitToJSValue(type, ValueLoc::flat(flat))) {
      return false;
    }
    localSet(jsParam);
  }

  // Results with more than one flat value are stored to a pointer that is
  // passed as the last parameter.
  bool useRetPtr =
      coreType.results().empty() && coreType.args().length() > nextFlatParam;
  uint32_t retPtr = nextFlatParam;
  mozilla::Maybe<ValType> flatResultType;
  if (!coreType.results().empty()) {
    flatResultType.emplace(coreType.results()[0]);
  }
  uint32_t jsResult = UINT32_MAX;
  uint32_t flatResult = UINT32_MAX;
  if (funcType.resultType) {
    jsResult = addLocal(ValType(RefType::extern_()));
  }
  if (flatResultType) {
    flatResult = addLocal(*flatResultType);
  }

  auto emitArgs = [&]() {
    for (uint32_t jsParam : jsParams) {
      localGet(jsParam);
    }
  };
  auto emitReturn = [&]() {
    if (flatResultType) {
      localGet(flatResult);
    }
    op(Op::Return);
  };
  ValueLoc loc = useRetPtr ? ValueLoc::memory(retPtr, 0)
                           : ValueLoc::flat(mozilla::Span(&flatResult, 1));

  if (funcType.resultType &&
      funcType.resultType->kind() == ComponentTypeKind::Result) {
    if (!emitLoweredResultCall(
            *funcType.resultType, hostIndex, loc,
            useRetPtr ? mozilla::Some(retPtr) : mozilla::Nothing(), emitArgs,
            emitReturn)) {
      return false;
    }
    emitReturn();
    return true;
  }

  block(Op::Block);
  tryTableCatchAll(0, flatResultType);
  emitArgs();
  call(hostIndex);
  if (funcType.resultType) {
    localSet(jsResult);
    if (useRetPtr) {
      emitCheckPointer(*funcType.resultType, retPtr);
    }
    if (!emitToComponentValue(*funcType.resultType, jsResult, loc)) {
      return false;
    }
  }
  if (flatResultType) {
    localGet(flatResult);
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
    const CanonOptValues& opts, MutableHandleFunction result) {
  LiftLowerBuilder b(cx, /*enterGuest=*/true);
  uint32_t calleeIndex, postReturn;
  if (!b.init(instanceObj) || !b.importWasmFunc(callee, &calleeIndex) ||
      !b.importCanonOpts(opts, &postReturn)) {
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

  if (!b.emitLiftAdapter(funcType, calleeIndex, postReturn)) {
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

// The JS-API's "create a component error". Always returns false.
static bool ThrowComponentError(JSContext* cx, HandleValue data) {
  JSObject* ctor =
      GlobalObject::getOrCreateConstructor(cx, JSProto_ComponentError);
  if (!ctor) {
    return false;
  }
  RootedValue ctorValue(cx, ObjectValue(*ctor));
  ConstructArgs ctorArgs(cx);
  if (!ctorArgs.init(cx, 2)) {
    return false;
  }
  ctorArgs[0].setString(cx->names().empty_);
  ctorArgs[1].set(data);
  RootedObject error(cx);
  if (!Construct(cx, ctorValue, ctorArgs, ctorValue, &error)) {
    return false;
  }
  RootedValue errorValue(cx, ObjectValue(*error));
  JS_SetPendingException(cx, errorValue);
  return false;
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

  // A call that fails after entering the guest locks down the instance, even
  // if the failure is not a trap. Calls into the instance from an import can
  // nest, so the outer call's state is restored afterwards.
  bool outerEnteredGuest = instance.enteredGuest();
  RootedValue outerErrorData(cx);
  bool outerHasError = instance.takePendingError(&outerErrorData);
  instance.setEnteredGuest(false);

  bool ok = Call(cx, adapter, UndefinedHandleValue, adapterArgs, args.rval());
  if (!ok && (instance.enteredGuest() || IsPendingTrap(cx))) {
    instance.setTrapped();
  }
  RootedValue errorData(cx);
  if (instance.takePendingError(&errorData) && ok) {
    ok = ThrowComponentError(cx, errorData);
  }

  instance.setEnteredGuest(outerEnteredGuest);
  if (outerHasError) {
    instance.setPendingError(outerErrorData);
  }
  return ok;
}

bool wasm::CreateLiftedFunction(
    JSContext* cx, Handle<WasmComponentInstanceObject*> instanceObj,
    const ComponentFuncType& funcType, HandleFunction callee,
    const CanonOptValues& opts, Handle<JSAtom*> name,
    MutableHandleFunction result) {
  if (!CheckSupportedAdapterFuncType(cx, funcType)) {
    return false;
  }

  RootedFunction adapter(cx);
  if (!GenerateLiftAdapter(cx, instanceObj, funcType, callee, opts, &adapter)) {
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
    HandleObject host, const CanonOptValues& opts,
    MutableHandleFunction result) {
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

  LiftLowerBuilder b(cx, /*enterGuest=*/false);
  uint32_t hostIndex, postReturn;
  if (!b.init(instanceObj) ||
      !b.importJSFunc(host, std::move(hostParams), std::move(hostResults),
                      &hostIndex) ||
      !b.importCanonOpts(opts, &postReturn) ||
      !b.defineAdapterFuncWithType(coreFuncType)) {
    return false;
  }
  MOZ_ASSERT(postReturn == UINT32_MAX);

  if (!b.emitLowerAdapter(funcType, coreFuncType.funcType(), hostIndex)) {
    ReportOutOfMemory(cx);
    return false;
  }
  return b.finish(result);
}

#endif  // ENABLE_WASM_COMPONENTS
