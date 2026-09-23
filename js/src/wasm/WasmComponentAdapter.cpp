/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "wasm/WasmComponentAdapter.h"

#ifdef ENABLE_WASM_COMPONENTS

#  include "mozilla/Casting.h"

#  include <cmath>

#  include "builtin/String.h"
#  include "js/CallAndConstruct.h"
#  include "js/Conversions.h"
#  include "js/experimental/TypedData.h"
#  include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#  include "util/Unicode.h"
#  include "vm/BigIntType.h"
#  include "vm/ErrorObject.h"
#  include "vm/JSFunction.h"
#  include "vm/TypedArrayObject.h"
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

// The tag on the length of a latin1+utf16 string that is stored as UTF-16.
static constexpr uint32_t UTF16Tag = 1u << 31;

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

  JSContext* cx() const { return cx_; }

  const TypeContext& types() const { return *codeMeta_->types; }

  // Imports a wasm function with its own type. Function imports must all be
  // added before the adapter function.
  [[nodiscard]] bool importWasmFunc(HandleFunction func, uint32_t* funcIndex);

  // Imports a function that is not a wasm function, which is called like a JS
  // function with this signature.
  [[nodiscard]] bool importJSFunc(HandleObject func, ValTypeVector&& params,
                                  ValTypeVector&& results, uint32_t* funcIndex);

  // Imports the memory for the canonical ABI as memory 0.
  [[nodiscard]] bool importMemory(Handle<WasmMemoryObject*> memory);

  // Imports a string as an immutable externref global, for the adapter to
  // pass to builtins. A string is only imported once.
  [[nodiscard]] bool importString(JSString* str, uint32_t* globalIndex);

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

bool AdapterModuleBuilder::importMemory(Handle<WasmMemoryObject*> memory) {
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

bool AdapterModuleBuilder::importString(JSString* str, uint32_t* globalIndex) {
  ValVector& values = imports_.get().globalValues;
  for (size_t i = 0; i < values.length(); i++) {
    JS::Value value = values[i].ref().toJSValue();
    if (value.isString() && value.toString() == str) {
      *globalIndex = i;
      return true;
    }
  }

  *globalIndex = codeMeta_->globals.length();
  GlobalType type(ValType(RefType::extern_()), /*isMutable=*/false);
  if (!codeMeta_->globals.append(GlobalDesc(type, *globalIndex)) ||
      !moduleMeta_->imports.emplaceBack(CacheableName(), CacheableName(),
                                        DefinitionKind::Global) ||
      !values.append(
          Val(ValType(RefType::extern_()), AnyRef::fromJSString(str)))) {
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
  [[nodiscard]] bool globalGet(uint32_t index) {
    return encoder_.writeOp(Op::GlobalGet) && encoder_.writeVarU32(index);
  }
  [[nodiscard]] bool ctx() { return globalGet(CtxGlobalIndex); }
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

  [[nodiscard]] bool br(uint32_t depth) {
    return encoder_.writeOp(Op::Br) && encoder_.writeVarU32(depth);
  }
  [[nodiscard]] bool brIf(uint32_t depth) {
    return encoder_.writeOp(Op::BrIf) && encoder_.writeVarU32(depth);
  }

  // Loads and stores from memory 0.
  [[nodiscard]] bool memoryAccess(Op op, uint32_t alignLog2, uint32_t offset) {
    return encoder_.writeOp(op) && encoder_.writeVarU32(alignLog2) &&
           encoder_.writeVarU32(offset);
  }

  [[nodiscard]] bool trap(AdapterTrap reason) {
    return i32Const(int32_t(reason)) &&
           callBuiltin(BuiltinModuleFuncId::CmTrap) && op(Op::Unreachable);
  }

  // Calls a function of the guest, trapping if it throws. `emitArgs` emits
  // the arguments.
  template <typename F>
  [[nodiscard]] bool callGuest(uint32_t funcIndex,
                               mozilla::Maybe<ValType> result, F emitArgs) {
    return block(Op::Block, result) && block(Op::Block, mozilla::Nothing()) &&
           tryTableCatchAll(result, 0) && emitArgs() && call(funcIndex) &&
           op(Op::End) && br(1) && op(Op::End) &&
           trap(AdapterTrap::UncaughtException) && op(Op::End);
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
    case ComponentTypeKind::String:
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

// The core wasm types a component value is flattened to, stored in locals.
using FlatLocals = Vector<uint32_t, 2, SystemAllocPolicy>;

[[nodiscard]] static bool FlatTypes(const ComponentType& type,
                                    ValTypeVector* flat) {
  bool hasStringsOrLists, tooDeep;
  return FlattenType(type, flat, &hasStringsOrLists, &tooDeep, 0);
}

[[nodiscard]] static bool AddFlatLocals(AdapterFuncEncoder& f,
                                        const ValTypeVector& flatTypes,
                                        FlatLocals* locals) {
  for (ValType flatType : flatTypes) {
    uint32_t index;
    if (!f.addLocal(flatType, &index) || !locals->append(index)) {
      return false;
    }
  }
  return true;
}

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

// The class of the typed array that a list of scalars of type `kind` is
// represented by, if it is represented by one.
static mozilla::Maybe<Scalar::Type> ScalarListArrayType(
    ComponentTypeKind kind) {
  switch (kind) {
    case ComponentTypeKind::S8:
      return mozilla::Some(Scalar::Int8);
    case ComponentTypeKind::U8:
      return mozilla::Some(Scalar::Uint8);
    case ComponentTypeKind::S16:
      return mozilla::Some(Scalar::Int16);
    case ComponentTypeKind::U16:
      return mozilla::Some(Scalar::Uint16);
    case ComponentTypeKind::S32:
      return mozilla::Some(Scalar::Int32);
    case ComponentTypeKind::U32:
      return mozilla::Some(Scalar::Uint32);
    case ComponentTypeKind::S64:
      return mozilla::Some(Scalar::BigInt64);
    case ComponentTypeKind::U64:
      return mozilla::Some(Scalar::BigUint64);
    case ComponentTypeKind::F32:
      return mozilla::Some(Scalar::Float32);
    case ComponentTypeKind::F64:
      return mozilla::Some(Scalar::Float64);
    default:
      return mozilla::Nothing();
  }
}

// The canonical ABI's canonical NaNs.
static constexpr uint32_t CanonicalFloat32NaN = 0x7fc00000;
static constexpr uint64_t CanonicalFloat64NaN = 0x7ff8000000000000;

// State shared by the code converting the values of one adapter.
struct AdapterContext {
  AdapterModuleBuilder& builder;
  AdapterFuncEncoder& f;
  ComponentStringEncoding stringEncoding;
  // The realloc function, or UINT32_MAX if there is none.
  uint32_t realloc;
  // Whether calls to realloc enter the guest, which is true when lifting.
  bool enterGuest;
};

// Pushes a scalar from `loc`.
[[nodiscard]] static bool EmitGetScalar(AdapterFuncEncoder& f,
                                        ComponentTypeKind kind,
                                        const ValueLoc& loc) {
  if (!loc.inMemory()) {
    return f.localGet(loc.flatLocals()[0]);
  }
  ScalarLayout layout = GetScalarLayout(kind);
  return f.localGet(loc.ptrLocal()) &&
         f.memoryAccess(layout.load, layout.alignLog2, loc.offset());
}

// Stores the scalar on the stack to `loc`.
[[nodiscard]] static bool EmitSetScalar(AdapterFuncEncoder& f,
                                        ComponentTypeKind kind,
                                        const ValueLoc& loc) {
  if (!loc.inMemory()) {
    return f.localSet(loc.flatLocals()[0]);
  }
  ScalarLayout layout = GetScalarLayout(kind);
  uint32_t value;
  return f.addLocal(layout.flatType, &value) && f.localSet(value) &&
         f.localGet(loc.ptrLocal()) && f.localGet(value) &&
         f.memoryAccess(layout.store, layout.alignLog2, loc.offset());
}

// Pushes the i32 at `index` of a value made of i32s, like a string.
[[nodiscard]] static bool EmitGetI32(AdapterFuncEncoder& f, const ValueLoc& loc,
                                     uint32_t index) {
  if (!loc.inMemory()) {
    return f.localGet(loc.flatLocals()[index]);
  }
  return f.localGet(loc.ptrLocal()) &&
         f.memoryAccess(Op::I32Load, 2, loc.offset() + 4 * index);
}

// Stores the i32 in local `value` at `index` of a value made of i32s.
[[nodiscard]] static bool EmitSetI32(AdapterFuncEncoder& f, const ValueLoc& loc,
                                     uint32_t index, uint32_t value) {
  if (!loc.inMemory()) {
    return f.localGet(value) && f.localSet(loc.flatLocals()[index]);
  }
  return f.localGet(loc.ptrLocal()) && f.localGet(value) &&
         f.memoryAccess(Op::I32Store, 2, loc.offset() + 4 * index);
}

// Calls realloc, with `emitOld` emitting its first two arguments, and stores
// the result in local `ptr`.
template <typename F>
[[nodiscard]] static bool EmitRealloc(AdapterContext& a, F emitOld,
                                      int32_t alignment, uint32_t newSize,
                                      uint32_t ptr) {
  AdapterFuncEncoder& f = a.f;
  MOZ_ASSERT(a.realloc != UINT32_MAX);
  return (!a.enterGuest ||
          (f.ctx() && f.callBuiltin(BuiltinModuleFuncId::CmEnterGuest))) &&
         f.callGuest(a.realloc, mozilla::Some(ValType::I32),
                     [&]() {
                       return emitOld() && f.i32Const(alignment) &&
                              f.localGet(newSize);
                     }) &&
         f.localSet(ptr);
}

// The canonical ABI's allocate, of the size in local `size`.
[[nodiscard]] static bool EmitAllocate(AdapterContext& a, int32_t alignment,
                                       uint32_t size, uint32_t ptr) {
  return EmitRealloc(
      a, [&]() { return a.f.i32Const(0) && a.f.i32Const(0); }, alignment, size,
      ptr);
}

// The canonical ABI's reallocate, of `ptr` from `oldSize` to `newSize`, all
// locals.
[[nodiscard]] static bool EmitReallocate(AdapterContext& a, uint32_t oldSize,
                                         int32_t alignment, uint32_t newSize,
                                         uint32_t ptr) {
  return EmitRealloc(
      a, [&]() { return a.f.localGet(ptr) && a.f.localGet(oldSize); },
      alignment, newSize, ptr);
}

// ToComponentValue for a string, with the canonical ABI's store_string. JS
// strings are stored as if their encoding is UTF-16.
[[nodiscard]] static bool EmitStoreString(AdapterContext& a, uint32_t jsValue,
                                          const ValueLoc& loc) {
  using Id = BuiltinModuleFuncId;
  AdapterFuncEncoder& f = a.f;
  uint32_t str, length, ptr, byteLength;
  if (!f.addLocal(ValType(RefType::extern_()), &str) ||
      !f.addLocal(ValType::I32, &length) || !f.addLocal(ValType::I32, &ptr) ||
      !f.addLocal(ValType::I32, &byteLength) || !f.localGet(jsValue) ||
      !f.callBuiltin(Id::CmToString) || !f.localSet(str) || !f.localGet(str) ||
      !f.callBuiltin(Id::CmStringLength) || !f.localSet(length)) {
    return false;
  }

  switch (a.stringEncoding) {
    case ComponentStringEncoding::UTF8: {
      // Optimistically allocate one byte per code unit, and if the string
      // turns out not to be ASCII, grow to the worst case and then shrink.
      uint32_t asciiLength;
      if (!f.addLocal(ValType::I32, &asciiLength) ||
          !EmitAllocate(a, 1, length, ptr) || !f.localGet(str) ||
          !f.localGet(ptr) || !f.localGet(length) ||
          !f.callBuiltin(Id::CmStoreASCII) || !f.localSet(asciiLength) ||
          !f.localGet(asciiLength) || !f.localGet(length) || !f.op(Op::I32Ne) ||
          !f.block(Op::If, mozilla::Nothing())) {
        return false;
      }
      uint32_t worstCase;
      if (!f.addLocal(ValType::I32, &worstCase) || !f.localGet(length) ||
          !f.i32Const(3) || !f.op(Op::I32Mul) || !f.localSet(worstCase) ||
          !EmitReallocate(a, length, 1, worstCase, ptr) || !f.ctx() ||
          !f.localGet(str) || !f.localGet(ptr) || !f.localGet(worstCase) ||
          !f.callBuiltin(Id::CmStoreUTF8) || !f.ctx() ||
          !f.callBuiltin(Id::CmScratchI32) || !f.localSet(byteLength) ||
          !f.localGet(worstCase) || !f.localGet(byteLength) ||
          !f.op(Op::I32GtU) || !f.block(Op::If, mozilla::Nothing()) ||
          !EmitReallocate(a, worstCase, 1, byteLength, ptr) ||
          !f.localGet(ptr) || !f.localGet(byteLength) || !f.i32Const(1) ||
          !f.i32Const(1) || !f.callBuiltin(Id::CmCheckRange) ||
          !f.op(Op::End) || !f.localGet(byteLength) || !f.localSet(length) ||
          !f.op(Op::End)) {
        return false;
      }
      break;
    }
    case ComponentStringEncoding::UTF16:
      if (!f.localGet(length) || !f.i32Const(1) || !f.op(Op::I32Shl) ||
          !f.localSet(byteLength) || !EmitAllocate(a, 2, byteLength, ptr) ||
          !f.localGet(str) || !f.localGet(ptr) || !f.localGet(length) ||
          !f.callBuiltin(Id::CmStoreUTF16)) {
        return false;
      }
      break;
    case ComponentStringEncoding::Latin1PlusUTF16: {
      // This is store_string_to_latin1_or_utf16. Because the source is UTF-16,
      // storing it as UTF-16 takes exactly two bytes per code unit, and so
      // neither of the reallocs that shrink the allocation happen.
      uint32_t latin1Length;
      if (!f.addLocal(ValType::I32, &latin1Length) ||
          !EmitAllocate(a, 2, length, ptr) || !f.localGet(str) ||
          !f.localGet(ptr) || !f.localGet(length) ||
          !f.callBuiltin(Id::CmStoreLatin1) || !f.localSet(latin1Length) ||
          !f.localGet(latin1Length) || !f.localGet(length) ||
          !f.op(Op::I32Ne) || !f.block(Op::If, mozilla::Nothing()) ||
          !f.localGet(length) || !f.i32Const(1) || !f.op(Op::I32Shl) ||
          !f.localSet(byteLength) ||
          !EmitReallocate(a, length, 2, byteLength, ptr) || !f.localGet(str) ||
          !f.localGet(ptr) || !f.localGet(latin1Length) ||
          !f.callBuiltin(Id::CmStoreLatin1ToUTF16) || !f.localGet(length) ||
          !f.i32Const(int32_t(UTF16Tag)) || !f.op(Op::I32Or) ||
          !f.localSet(length) || !f.op(Op::End)) {
        return false;
      }
      break;
    }
  }

  return EmitSetI32(f, loc, 0, ptr) && EmitSetI32(f, loc, 1, length);
}

[[nodiscard]] static bool EmitToComponentValue(AdapterContext& a,
                                               const ComponentType& type,
                                               uint32_t jsValue,
                                               const ValueLoc& loc);
[[nodiscard]] static bool EmitToJSValue(AdapterContext& a,
                                        const ComponentType& type,
                                        const ValueLoc& loc);

// Computes the location of each field of a tuple or record at `loc`.
[[nodiscard]] static bool GetFieldLocs(
    const ComponentTypeVector& fieldTypes, const ValueLoc& loc,
    Vector<ValueLoc, 8, SystemAllocPolicy>* fieldLocs) {
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

// Emits a loop over `i` from 0 to `count`, where `elemPtr` is `ptr` plus
// `i * elemSize`, with `emitBody` emitting the body.
template <typename F>
[[nodiscard]] static bool EmitListLoop(AdapterFuncEncoder& f, uint32_t ptr,
                                       uint32_t count, uint32_t elemSize,
                                       uint32_t i, uint32_t elemPtr,
                                       F emitBody) {
  return f.i32Const(0) && f.localSet(i) && f.localGet(ptr) &&
         f.localSet(elemPtr) && f.block(Op::Block, mozilla::Nothing()) &&
         f.block(Op::Loop, mozilla::Nothing()) && f.localGet(i) &&
         f.localGet(count) && f.op(Op::I32GeU) && f.brIf(1) && emitBody() &&
         f.localGet(elemPtr) && f.i32Const(int32_t(elemSize)) &&
         f.op(Op::I32Add) && f.localSet(elemPtr) && f.localGet(i) &&
         f.i32Const(1) && f.op(Op::I32Add) && f.localSet(i) && f.br(0) &&
         f.op(Op::End) && f.op(Op::End);
}

// ToComponentValue for a tuple: as for a list, which must have exactly as many
// elements as the tuple.
[[nodiscard]] static bool EmitToComponentValueTuple(AdapterContext& a,
                                                    const ComponentType& type,
                                                    uint32_t jsValue,
                                                    const ValueLoc& loc) {
  using Id = BuiltinModuleFuncId;
  AdapterFuncEncoder& f = a.f;
  const ComponentTypeVector& itemTypes = type.asTuple();
  Vector<ValueLoc, 8, SystemAllocPolicy> itemLocs;
  uint32_t array;
  if (!GetFieldLocs(itemTypes, loc, &itemLocs) ||
      !f.addLocal(ValType(RefType::extern_()), &array) ||
      !f.localGet(jsValue) || !f.i32Const(0) ||
      !f.callBuiltin(Id::CmIterableToArray) || !f.localSet(array) ||
      !f.localGet(array) || !f.i32Const(int32_t(itemTypes.length())) ||
      !f.callBuiltin(Id::CmCheckArrayLength)) {
    return false;
  }
  for (size_t i = 0; i < itemTypes.length(); i++) {
    uint32_t item;
    if (!f.addLocal(ValType(RefType::extern_()), &item) || !f.localGet(array) ||
        !f.i32Const(int32_t(i)) || !f.callBuiltin(Id::CmArrayGet) ||
        !f.localSet(item) ||
        !EmitToComponentValue(a, itemTypes[i], item, itemLocs[i])) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] static bool EmitToJSValueTuple(AdapterContext& a,
                                             const ComponentType& type,
                                             const ValueLoc& loc) {
  using Id = BuiltinModuleFuncId;
  AdapterFuncEncoder& f = a.f;
  const ComponentTypeVector& itemTypes = type.asTuple();
  Vector<ValueLoc, 8, SystemAllocPolicy> itemLocs;
  uint32_t array;
  if (!GetFieldLocs(itemTypes, loc, &itemLocs) ||
      !f.addLocal(ValType(RefType::extern_()), &array) ||
      !f.i32Const(int32_t(itemTypes.length())) ||
      !f.callBuiltin(Id::CmNewArray) || !f.localSet(array)) {
    return false;
  }
  for (size_t i = 0; i < itemTypes.length(); i++) {
    if (!f.localGet(array) || !f.i32Const(int32_t(i)) ||
        !EmitToJSValue(a, itemTypes[i], itemLocs[i]) ||
        !f.callBuiltin(Id::CmArraySet)) {
      return false;
    }
  }
  return f.localGet(array);
}

// Pushes the JS name of a label.
[[nodiscard]] static bool EmitName(AdapterContext& a,
                                   const CacheableName& label,
                                   ComponentJSCase jsCase) {
  JSAtom* atom = ComponentLabelToJS(a.builder.cx(), label.utf8Bytes(), jsCase);
  uint32_t global;
  return atom && a.builder.importString(atom, &global) && a.f.globalGet(global);
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

// The JS-API's ToComponentValueRecord.
[[nodiscard]] static bool EmitToComponentValueRecord(AdapterContext& a,
                                                     const ComponentType& type,
                                                     uint32_t jsValue,
                                                     const ValueLoc& loc) {
  using Id = BuiltinModuleFuncId;
  AdapterFuncEncoder& f = a.f;
  const ComponentRecordFieldVector& fields = type.asRecord();
  ComponentTypeVector fieldTypes;
  Vector<ValueLoc, 8, SystemAllocPolicy> fieldLocs;
  if (!GetRecordFieldTypes(type, &fieldTypes) ||
      !GetFieldLocs(fieldTypes, loc, &fieldLocs) || !f.localGet(jsValue) ||
      !f.callBuiltin(Id::CmCheckObject)) {
    return false;
  }
  for (size_t i = 0; i < fields.length(); i++) {
    uint32_t field;
    if (!f.addLocal(ValType(RefType::extern_()), &field) ||
        !f.localGet(jsValue) ||
        !EmitName(a, fields[i].name, ComponentJSCase::Camel) ||
        !f.callBuiltin(Id::CmGetField) || !f.localSet(field) ||
        !EmitToComponentValue(a, fields[i].type, field, fieldLocs[i])) {
      return false;
    }
  }
  return true;
}

// The JS-API's ToJSValueRecord.
[[nodiscard]] static bool EmitToJSValueRecord(AdapterContext& a,
                                              const ComponentType& type,
                                              const ValueLoc& loc) {
  using Id = BuiltinModuleFuncId;
  AdapterFuncEncoder& f = a.f;
  const ComponentRecordFieldVector& fields = type.asRecord();
  ComponentTypeVector fieldTypes;
  Vector<ValueLoc, 8, SystemAllocPolicy> fieldLocs;
  uint32_t record;
  if (!GetRecordFieldTypes(type, &fieldTypes) ||
      !GetFieldLocs(fieldTypes, loc, &fieldLocs) ||
      !f.addLocal(ValType(RefType::extern_()), &record) ||
      !f.callBuiltin(Id::CmNewRecord) || !f.localSet(record)) {
    return false;
  }
  for (size_t i = 0; i < fields.length(); i++) {
    if (!f.localGet(record) ||
        !EmitName(a, fields[i].name, ComponentJSCase::Camel) ||
        !EmitToJSValue(a, fields[i].type, fieldLocs[i]) ||
        !f.callBuiltin(Id::CmDefineField)) {
      return false;
    }
  }
  return f.localGet(record);
}

// ToComponentValue for a list, with the canonical ABI's store_list.
[[nodiscard]] static bool EmitToComponentValueList(AdapterContext& a,
                                                   const ComponentType& type,
                                                   uint32_t jsValue,
                                                   const ValueLoc& loc) {
  using Id = BuiltinModuleFuncId;
  AdapterFuncEncoder& f = a.f;
  ComponentType elemType = type.asList();
  uint32_t elemSize = CanonElemSize(elemType);
  uint32_t alignment = CanonAlignment(elemType);
  uint32_t ptr, length, byteLength;
  if (!f.addLocal(ValType::I32, &ptr) || !f.addLocal(ValType::I32, &length) ||
      !f.addLocal(ValType::I32, &byteLength) ||
      !f.block(Op::Block, mozilla::Nothing())) {
    return false;
  }

  // A typed array of the list's scalar type is copied directly.
  int32_t kind = int32_t(elemType.kind());
  if (ScalarListArrayType(elemType.kind()) &&
      (!f.localGet(jsValue) || !f.i32Const(kind) ||
       !f.callBuiltin(Id::CmIsScalarArray) ||
       !f.block(Op::If, mozilla::Nothing()) || !f.localGet(jsValue) ||
       !f.i32Const(kind) || !f.callBuiltin(Id::CmScalarArrayLength) ||
       !f.localSet(length) || !f.localGet(length) ||
       !f.i32Const(int32_t(elemSize)) || !f.op(Op::I32Mul) ||
       !f.localSet(byteLength) ||
       !EmitAllocate(a, int32_t(alignment), byteLength, ptr) ||
       !f.localGet(jsValue) || !f.localGet(ptr) || !f.localGet(length) ||
       !f.callBuiltin(Id::CmCopyScalars) || !f.br(1) || !f.op(Op::End))) {
    return false;
  }

  uint32_t array, i, elemPtr, elem;
  if (!f.addLocal(ValType(RefType::extern_()), &array) ||
      !f.addLocal(ValType::I32, &i) || !f.addLocal(ValType::I32, &elemPtr) ||
      !f.addLocal(ValType(RefType::extern_()), &elem) || !f.localGet(jsValue) ||
      !f.i32Const(int32_t(elemSize)) || !f.callBuiltin(Id::CmIterableToArray) ||
      !f.localSet(array) || !f.localGet(array) ||
      !f.callBuiltin(Id::CmArrayLength) || !f.localSet(length) ||
      !f.localGet(length) || !f.i32Const(int32_t(elemSize)) ||
      !f.op(Op::I32Mul) || !f.localSet(byteLength) ||
      !EmitAllocate(a, int32_t(alignment), byteLength, ptr) ||
      !f.localGet(ptr) || !f.localGet(length) ||
      !f.i32Const(int32_t(elemSize)) || !f.i32Const(int32_t(alignment)) ||
      !f.callBuiltin(Id::CmCheckRange)) {
    return false;
  }
  if (!EmitListLoop(f, ptr, length, elemSize, i, elemPtr, [&]() {
        return f.localGet(array) && f.localGet(i) &&
               f.callBuiltin(Id::CmArrayGet) && f.localSet(elem) &&
               EmitToComponentValue(a, elemType, elem,
                                    ValueLoc::memory(elemPtr, 0));
      })) {
    return false;
  }

  return f.op(Op::End) && EmitSetI32(f, loc, 0, ptr) &&
         EmitSetI32(f, loc, 1, length);
}

// ToJSValue for a list, with the canonical ABI's load_list.
[[nodiscard]] static bool EmitToJSValueList(AdapterContext& a,
                                            const ComponentType& type,
                                            const ValueLoc& loc) {
  using Id = BuiltinModuleFuncId;
  AdapterFuncEncoder& f = a.f;
  ComponentType elemType = type.asList();

  // A list of scalars becomes a typed array.
  if (ScalarListArrayType(elemType.kind())) {
    return EmitGetI32(f, loc, 0) && EmitGetI32(f, loc, 1) &&
           f.i32Const(int32_t(elemType.kind())) &&
           f.callBuiltin(Id::CmScalarListToJS);
  }

  uint32_t elemSize = CanonElemSize(elemType);
  uint32_t ptr, length, array, i, elemPtr;
  return f.addLocal(ValType::I32, &ptr) && f.addLocal(ValType::I32, &length) &&
         f.addLocal(ValType(RefType::extern_()), &array) &&
         f.addLocal(ValType::I32, &i) && f.addLocal(ValType::I32, &elemPtr) &&
         EmitGetI32(f, loc, 0) && f.localSet(ptr) && EmitGetI32(f, loc, 1) &&
         f.localSet(length) && f.localGet(ptr) && f.localGet(length) &&
         f.i32Const(int32_t(elemSize)) &&
         f.i32Const(int32_t(CanonAlignment(elemType))) &&
         f.callBuiltin(Id::CmCheckRange) && f.localGet(length) &&
         f.callBuiltin(Id::CmNewArray) && f.localSet(array) &&
         EmitListLoop(f, ptr, length, elemSize, i, elemPtr,
                      [&]() {
                        return f.localGet(array) && f.localGet(i) &&
                               EmitToJSValue(a, elemType,
                                             ValueLoc::memory(elemPtr, 0)) &&
                               f.callBuiltin(Id::CmArraySet);
                      }) &&
         f.localGet(array);
}

// Emits ToComponentValue for the JS value in local `jsValue`, storing the
// component value to `loc`.
[[nodiscard]] static bool EmitToComponentValue(AdapterContext& a,
                                               const ComponentType& type,
                                               uint32_t jsValue,
                                               const ValueLoc& loc) {
  using Id = BuiltinModuleFuncId;
  AdapterFuncEncoder& f = a.f;
  ComponentTypeKind kind = type.kind();
  switch (kind) {
    case ComponentTypeKind::Bool:
      return f.localGet(jsValue) && f.callBuiltin(Id::CmToBool) &&
             EmitSetScalar(f, kind, loc);
    case ComponentTypeKind::S64:
    case ComponentTypeKind::U64:
      return f.ctx() && f.localGet(jsValue) && f.i32Const(int32_t(kind)) &&
             f.callBuiltin(Id::CmToInteger) && f.ctx() &&
             f.callBuiltin(Id::CmScratchI64) && EmitSetScalar(f, kind, loc);
    case ComponentTypeKind::F32:
    case ComponentTypeKind::F64:
      return f.ctx() && f.localGet(jsValue) && f.callBuiltin(Id::CmToFloat) &&
             f.ctx() && f.callBuiltin(Id::CmScratchF64) &&
             (kind == ComponentTypeKind::F64 || f.op(Op::F32DemoteF64)) &&
             EmitSetScalar(f, kind, loc);
    case ComponentTypeKind::Char:
      return f.localGet(jsValue) && f.callBuiltin(Id::CmToChar) &&
             EmitSetScalar(f, kind, loc);
    case ComponentTypeKind::String:
      return EmitStoreString(a, jsValue, loc);
    case ComponentTypeKind::List:
      return EmitToComponentValueList(a, type, jsValue, loc);
    case ComponentTypeKind::Tuple:
      return EmitToComponentValueTuple(a, type, jsValue, loc);
    case ComponentTypeKind::Record:
      return EmitToComponentValueRecord(a, type, jsValue, loc);
    default:
      MOZ_ASSERT(IsIntegerKind(kind));
      return f.ctx() && f.localGet(jsValue) && f.i32Const(int32_t(kind)) &&
             f.callBuiltin(Id::CmToInteger) && f.ctx() &&
             f.callBuiltin(Id::CmScratchI32) && EmitSetScalar(f, kind, loc);
  }
}

// Emits ToJSValue for the component value at `loc`, leaving the JS value on
// the stack.
[[nodiscard]] static bool EmitToJSValue(AdapterContext& a,
                                        const ComponentType& type,
                                        const ValueLoc& loc) {
  using Id = BuiltinModuleFuncId;
  AdapterFuncEncoder& f = a.f;
  ComponentTypeKind kind = type.kind();
  switch (kind) {
    case ComponentTypeKind::Bool:
      return EmitGetScalar(f, kind, loc) && f.callBuiltin(Id::CmBoolToJS);
    case ComponentTypeKind::S64:
    case ComponentTypeKind::U64:
      return EmitGetScalar(f, kind, loc) && f.i32Const(int32_t(kind)) &&
             f.callBuiltin(Id::CmInt64ToJS);
    case ComponentTypeKind::F32:
      return EmitGetScalar(f, kind, loc) && f.op(Op::F64PromoteF32) &&
             f.callBuiltin(Id::CmFloatToJS);
    case ComponentTypeKind::F64:
      return EmitGetScalar(f, kind, loc) && f.callBuiltin(Id::CmFloatToJS);
    case ComponentTypeKind::Char:
      return EmitGetScalar(f, kind, loc) && f.callBuiltin(Id::CmCharToJS);
    case ComponentTypeKind::String:
      return EmitGetI32(f, loc, 0) && EmitGetI32(f, loc, 1) &&
             f.i32Const(int32_t(a.stringEncoding)) &&
             f.callBuiltin(Id::CmStringToJS);
    case ComponentTypeKind::List:
      return EmitToJSValueList(a, type, loc);
    case ComponentTypeKind::Tuple:
      return EmitToJSValueTuple(a, type, loc);
    case ComponentTypeKind::Record:
      return EmitToJSValueRecord(a, type, loc);
    default:
      MOZ_ASSERT(IsIntegerKind(kind));
      return EmitGetScalar(f, kind, loc) && f.i32Const(int32_t(kind)) &&
             f.callBuiltin(Id::CmIntegerToJS);
  }
}

// Traps unless `ptr` is aligned for `type` and all of `type` fits in memory.
[[nodiscard]] static bool EmitCheckPointer(AdapterFuncEncoder& f,
                                           const ComponentType& type,
                                           uint32_t ptr) {
  return f.localGet(ptr) && f.i32Const(1) &&
         f.i32Const(int32_t(CanonElemSize(type))) &&
         f.i32Const(int32_t(CanonAlignment(type))) &&
         f.callBuiltin(BuiltinModuleFuncId::CmCheckRange);
}

// Imports the memory and functions of the canonical options into `builder`,
// and sets up `realloc` and `postReturn` with their function indices, or
// UINT32_MAX if they are absent.
[[nodiscard]] static bool ImportCanonOpts(AdapterModuleBuilder& builder,
                                          const CanonOptValues& opts,
                                          uint32_t* realloc,
                                          uint32_t* postReturn) {
  *realloc = UINT32_MAX;
  *postReturn = UINT32_MAX;
  return (!opts.memory || builder.importMemory(opts.memory)) &&
         (!opts.realloc || builder.importWasmFunc(opts.realloc, realloc)) &&
         (!opts.postReturn ||
          builder.importWasmFunc(opts.postReturn, postReturn));
}

// Generates the adapter for a lifted function, which takes one externref for
// each parameter and returns an externref if the function has a result:
//
//   (func (param externref)* (result externref)?
//     ;; ToComponentValue for each parameter, into flat locals
//     ;; call $callee, trapping if it throws
//     ;; ToJSValue for the result, from flat locals or the result pointer
//     ;; call $post-return, if any
//   )
[[nodiscard]] static bool GenerateLiftAdapter(
    JSContext* cx, Handle<WasmComponentInstanceObject*> instanceObj,
    const ComponentFuncType& funcType, HandleFunction callee,
    const CanonOptValues& opts, MutableHandleFunction result) {
  AdapterModuleBuilder builder(cx);
  uint32_t calleeIndex, realloc, postReturn;
  if (!builder.init(instanceObj) ||
      !builder.importWasmFunc(callee, &calleeIndex) ||
      !ImportCanonOpts(builder, opts, &realloc, &postReturn)) {
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
  AdapterContext a{builder, f, opts.stringEncoding, realloc,
                   /*enterGuest=*/true};

  FlatLocals flatArgs;
  for (uint32_t i = 0; i < numParams; i++) {
    const ComponentType& type = funcType.paramTypes[i];
    ValTypeVector flatTypes;
    size_t start = flatArgs.length();
    if (!FlatTypes(type, &flatTypes) ||
        !AddFlatLocals(f, flatTypes, &flatArgs) ||
        !EmitToComponentValue(
            a, type, i,
            ValueLoc::flat(
                mozilla::Span<const uint32_t>(flatArgs).From(start)))) {
      ReportOutOfMemory(cx);
      return false;
    }
  }

  // Results with more than one flat value are returned through a pointer.
  ValTypeVector flatResultTypes;
  if (funcType.resultType &&
      !FlatTypes(*funcType.resultType, &flatResultTypes)) {
    ReportOutOfMemory(cx);
    return false;
  }
  bool useRetPtr = flatResultTypes.length() > MaxFlatResults;
  mozilla::Maybe<ValType> calleeResultType;
  if (!flatResultTypes.empty()) {
    calleeResultType.emplace(useRetPtr ? ValType::I32 : flatResultTypes[0]);
  }
  uint32_t calleeResult = UINT32_MAX;
  if (calleeResultType && !f.addLocal(*calleeResultType, &calleeResult)) {
    ReportOutOfMemory(cx);
    return false;
  }

  bool ok = f.ctx() && f.callBuiltin(BuiltinModuleFuncId::CmEnterGuest) &&
            f.callGuest(calleeIndex, calleeResultType, [&]() {
              bool ok = true;
              for (uint32_t local : flatArgs) {
                ok = ok && f.localGet(local);
              }
              return ok;
            });
  if (calleeResultType) {
    ok = ok && f.localSet(calleeResult);
  }

  uint32_t jsResult = UINT32_MAX;
  if (funcType.resultType) {
    ValueLoc loc = useRetPtr ? ValueLoc::memory(calleeResult, 0)
                             : ValueLoc::flat(mozilla::Span(&calleeResult, 1));
    ok = ok && f.addLocal(ValType(RefType::extern_()), &jsResult) &&
         (!useRetPtr ||
          EmitCheckPointer(f, *funcType.resultType, calleeResult)) &&
         EmitToJSValue(a, *funcType.resultType, loc) && f.localSet(jsResult);
  }

  if (postReturn != UINT32_MAX) {
    ok = ok && f.callGuest(postReturn, mozilla::Nothing(), [&]() {
      return !calleeResultType || f.localGet(calleeResult);
    });
  }
  if (funcType.resultType) {
    ok = ok && f.localGet(jsResult);
  }

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

  // A call that fails after entering the guest locks down the instance, even
  // if the failure is not a trap. Calls into the instance from an import can
  // nest, so the outer call's state is restored afterwards.
  bool outerEnteredGuest = instance.enteredGuest();
  instance.setEnteredGuest(false);
  bool ok = Call(cx, adapter, UndefinedHandleValue, adapterArgs, args.rval());
  if (!ok && (instance.enteredGuest() || IsPendingTrap(cx))) {
    instance.setTrapped();
  }
  instance.setEnteredGuest(outerEnteredGuest);
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

// Generates the adapter for a lowered function, which has the flattened core
// function type and calls the host function with one externref for each
// parameter:
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
bool wasm::CreateLoweredFunction(
    JSContext* cx, Handle<WasmComponentInstanceObject*> instanceObj,
    const ComponentFuncType& funcType, const TypeDef& coreFuncType,
    HandleObject host, const CanonOptValues& opts,
    MutableHandleFunction result) {
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
  uint32_t hostIndex, realloc, postReturn;
  if (!builder.init(instanceObj) ||
      !builder.importJSFunc(host, std::move(hostParams), std::move(hostResults),
                            &hostIndex) ||
      !ImportCanonOpts(builder, opts, &realloc, &postReturn) ||
      !builder.defineAdapterFuncWithType(coreFuncType)) {
    return false;
  }
  MOZ_ASSERT(postReturn == UINT32_MAX);

  const FuncType& coreType = coreFuncType.funcType();
  AdapterFuncEncoder f(builder.types(), coreType.args().length());
  AdapterContext a{builder, f, opts.stringEncoding, realloc,
                   /*enterGuest=*/false};

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
        !jsParams.append(jsParam) ||
        !EmitToJSValue(a, type, ValueLoc::flat(flat)) || !f.localSet(jsParam)) {
      ReportOutOfMemory(cx);
      return false;
    }
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
  if ((funcType.resultType &&
       !f.addLocal(ValType(RefType::extern_()), &jsResult)) ||
      (flatResultType && !f.addLocal(*flatResultType, &flatResult))) {
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
    ValueLoc loc = useRetPtr ? ValueLoc::memory(retPtr, 0)
                             : ValueLoc::flat(mozilla::Span(&flatResult, 1));
    ok = ok && f.localSet(jsResult) &&
         (!useRetPtr || EmitCheckPointer(f, *funcType.resultType, retPtr)) &&
         EmitToComponentValue(a, *funcType.resultType, jsResult, loc);
  }
  if (flatResultType) {
    ok = ok && f.localGet(flatResult);
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

void wasm::CmEnterGuest(Instance* instance, void* ctxRef) {
  MOZ_ASSERT(SASigCmEnterGuest.failureMode == FailureMode::Infallible);
  JSContext* cx = instance->cx();
  RootedObject ctx(cx, &AnyRef::fromCompiledCode(ctxRef).toJSObject());
  InstanceFromCtx(ctx).setEnteredGuest(true);
}

// Gets [ptr, ptr + length) in memory 0, or traps if it is out of bounds.
static uint8_t* GetMemoryRange(Instance* instance, uint32_t ptr,
                               uint64_t length) {
  uint64_t memoryLength = instance->memory(0)->volatileMemoryLength();
  if (uint64_t(ptr) + length > memoryLength) {
    ReportTrapError(instance->cx(), JSMSG_WASM_OUT_OF_BOUNDS);
    return nullptr;
  }
  return instance->memoryBase(0).unwrap() + ptr;
}

static bool CheckAlignment(Instance* instance, uint32_t ptr,
                           uint32_t alignment) {
  if (ptr % alignment != 0) {
    ReportTrapError(instance->cx(), JSMSG_WASM_UNALIGNED_ACCESS);
    return false;
  }
  return true;
}

int32_t wasm::CmCheckRange(Instance* instance, int32_t ptr, int32_t count,
                           int32_t elemSize, int32_t alignment) {
  MOZ_ASSERT(SASigCmCheckRange.failureMode == FailureMode::FailOnNegI32);
  if (!CheckAlignment(instance, uint32_t(ptr), uint32_t(alignment)) ||
      !GetMemoryRange(instance, uint32_t(ptr),
                      uint64_t(uint32_t(count)) * uint32_t(elemSize))) {
    return -1;
  }
  return 0;
}

void* wasm::CmIterableToArray(Instance* instance, void* valueRef,
                              int32_t elemSize) {
  MOZ_ASSERT(SASigCmIterableToArray.failureMode ==
             FailureMode::FailOnInvalidRef);
  JSContext* cx = instance->cx();
  RootedValue value(cx, AnyRef::fromCompiledCode(valueRef).toJSValue());
  if (!value.isObject()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_NOT_ITERABLE,
                              "value");
    return AnyRef::invalid().forCompiledCode();
  }
  Rooted<ArrayObject*> array(cx, IterableToArray(cx, value));
  if (!array) {
    return AnyRef::invalid().forCompiledCode();
  }
  // The list's size in bytes must fit in a 32-bit memory.
  if (uint64_t(array->length()) * uint32_t(elemSize) > UINT32_MAX) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_WASM_COMPONENT_LIST_TOO_LONG);
    return AnyRef::invalid().forCompiledCode();
  }
  RootedValue arrayValue(cx, ObjectValue(*array));
  return ExternRefForCompiledCode(cx, arrayValue);
}

static ArrayObject& ArrayFromRef(void* arrayRef) {
  return AnyRef::fromCompiledCode(arrayRef).toJSObject().as<ArrayObject>();
}

int32_t wasm::CmArrayLength(Instance* instance, void* arrayRef) {
  MOZ_ASSERT(SASigCmArrayLength.failureMode == FailureMode::Infallible);
  return int32_t(ArrayFromRef(arrayRef).length());
}

int32_t wasm::CmCheckArrayLength(Instance* instance, void* arrayRef,
                                 int32_t length) {
  MOZ_ASSERT(SASigCmCheckArrayLength.failureMode == FailureMode::FailOnNegI32);
  if (ArrayFromRef(arrayRef).length() != uint32_t(length)) {
    JS_ReportErrorNumberASCII(instance->cx(), GetErrorMessage, nullptr,
                              JSMSG_WASM_COMPONENT_BAD_LENGTH);
    return -1;
  }
  return 0;
}

void* wasm::CmArrayGet(Instance* instance, void* arrayRef, int32_t index) {
  MOZ_ASSERT(SASigCmArrayGet.failureMode == FailureMode::FailOnInvalidRef);
  JSContext* cx = instance->cx();
  // The array is from CmIterableToArray, so its elements are dense.
  RootedValue value(cx,
                    ArrayFromRef(arrayRef).getDenseElement(uint32_t(index)));
  return ExternRefForCompiledCode(cx, value);
}

void* wasm::CmNewArray(Instance* instance, int32_t length) {
  MOZ_ASSERT(SASigCmNewArray.failureMode == FailureMode::FailOnInvalidRef);
  JSContext* cx = instance->cx();
  ArrayObject* array = NewDenseFullyAllocatedArray(cx, uint32_t(length));
  if (!array) {
    return AnyRef::invalid().forCompiledCode();
  }
  RootedValue value(cx, ObjectValue(*array));
  return ExternRefForCompiledCode(cx, value);
}

int32_t wasm::CmArraySet(Instance* instance, void* arrayRef, int32_t index,
                         void* valueRef) {
  MOZ_ASSERT(SASigCmArraySet.failureMode == FailureMode::FailOnNegI32);
  JSContext* cx = instance->cx();
  Rooted<ArrayObject*> array(cx, &ArrayFromRef(arrayRef));
  RootedValue value(cx, AnyRef::fromCompiledCode(valueRef).toJSValue());
  if (!DefineDataElement(cx, array, uint32_t(index), value)) {
    return -1;
  }
  return 0;
}

int32_t wasm::CmCheckObject(Instance* instance, void* valueRef) {
  MOZ_ASSERT(SASigCmCheckObject.failureMode == FailureMode::FailOnNegI32);
  if (!AnyRef::fromCompiledCode(valueRef).toJSValue().isObject()) {
    JS_ReportErrorNumberASCII(instance->cx(), GetErrorMessage, nullptr,
                              JSMSG_OBJECT_REQUIRED, "value");
    return -1;
  }
  return 0;
}

void* wasm::CmGetField(Instance* instance, void* objectRef, void* nameRef) {
  MOZ_ASSERT(SASigCmGetField.failureMode == FailureMode::FailOnInvalidRef);
  JSContext* cx = instance->cx();
  RootedObject obj(cx, &AnyRef::fromCompiledCode(objectRef).toJSObject());
  RootedId name(
      cx,
      AtomToId(
          &AnyRef::fromCompiledCode(nameRef).toJSValue().toString()->asAtom()));
  RootedValue value(cx);
  if (!GetProperty(cx, obj, obj, name, &value)) {
    return AnyRef::invalid().forCompiledCode();
  }
  return ExternRefForCompiledCode(cx, value);
}

void* wasm::CmNewRecord(Instance* instance) {
  MOZ_ASSERT(SASigCmNewRecord.failureMode == FailureMode::FailOnInvalidRef);
  JSContext* cx = instance->cx();
  JSObject* obj = NewPlainObjectWithProto(cx, nullptr);
  if (!obj) {
    return AnyRef::invalid().forCompiledCode();
  }
  RootedValue value(cx, ObjectValue(*obj));
  return ExternRefForCompiledCode(cx, value);
}

int32_t wasm::CmDefineField(Instance* instance, void* objectRef, void* nameRef,
                            void* valueRef) {
  MOZ_ASSERT(SASigCmDefineField.failureMode == FailureMode::FailOnNegI32);
  JSContext* cx = instance->cx();
  RootedObject obj(cx, &AnyRef::fromCompiledCode(objectRef).toJSObject());
  RootedId name(
      cx,
      AtomToId(
          &AnyRef::fromCompiledCode(nameRef).toJSValue().toString()->asAtom()));
  RootedValue value(cx, AnyRef::fromCompiledCode(valueRef).toJSValue());
  if (!DefineDataProperty(cx, obj, name, value, JSPROP_ENUMERATE)) {
    return -1;
  }
  return 0;
}

static JSObject* NewScalarArray(JSContext* cx, Scalar::Type type,
                                size_t length) {
  switch (type) {
#  define NEW_ARRAY(ExternalType, NativeType, Name) \
    case Scalar::Name:                              \
      return JS_New##Name##Array(cx, length);
    JS_FOR_EACH_TYPED_ARRAY(NEW_ARRAY)
#  undef NEW_ARRAY
    default:
      MOZ_CRASH("not a typed array type");
  }
}

// Replaces NaNs with the canonical NaN, as the canonical ABI does when it
// loads a float.
template <typename T, typename Bits>
static void CanonicalizeNaNs(T* elems, size_t length, Bits canonicalNaN) {
  for (size_t i = 0; i < length; i++) {
    if (std::isnan(elems[i])) {
      elems[i] = mozilla::BitwiseCast<T>(canonicalNaN);
    }
  }
}

void* wasm::CmScalarListToJS(Instance* instance, int32_t ptr, int32_t length,
                             int32_t kind) {
  MOZ_ASSERT(SASigCmScalarListToJS.failureMode ==
             FailureMode::FailOnInvalidRef);
  JSContext* cx = instance->cx();
  Scalar::Type type = *ScalarListArrayType(ComponentTypeKind(kind));
  size_t elemSize = Scalar::byteSize(type);
  uint64_t byteLength = uint64_t(uint32_t(length)) * elemSize;
  if (!CheckAlignment(instance, uint32_t(ptr), elemSize)) {
    return AnyRef::invalid().forCompiledCode();
  }
  const uint8_t* src = GetMemoryRange(instance, uint32_t(ptr), byteLength);
  if (!src) {
    return AnyRef::invalid().forCompiledCode();
  }
  // Memory is not moved by GC, so `src` stays valid while allocating.
  JSObject* obj = NewScalarArray(cx, type, uint32_t(length));
  if (!obj) {
    return AnyRef::invalid().forCompiledCode();
  }
  TypedArrayObject& array = obj->as<TypedArrayObject>();
  void* dst = array.dataPointerUnshared();
  memcpy(dst, src, byteLength);
  if (type == Scalar::Float32) {
    CanonicalizeNaNs(static_cast<float*>(dst), uint32_t(length),
                     CanonicalFloat32NaN);
  } else if (type == Scalar::Float64) {
    CanonicalizeNaNs(static_cast<double*>(dst), uint32_t(length),
                     CanonicalFloat64NaN);
  }
  RootedValue value(cx, ObjectValue(array));
  return ExternRefForCompiledCode(cx, value);
}

// Gets a value as a typed array of the given type, or null.
static TypedArrayObject* AsScalarArray(const Value& value, int32_t kind) {
  if (!value.isObject() || !value.toObject().is<TypedArrayObject>()) {
    return nullptr;
  }
  TypedArrayObject& array = value.toObject().as<TypedArrayObject>();
  return array.type() == *ScalarListArrayType(ComponentTypeKind(kind))
             ? &array
             : nullptr;
}

int32_t wasm::CmIsScalarArray(Instance* instance, void* valueRef,
                              int32_t kind) {
  MOZ_ASSERT(SASigCmIsScalarArray.failureMode == FailureMode::Infallible);
  return !!AsScalarArray(AnyRef::fromCompiledCode(valueRef).toJSValue(), kind);
}

// Gets the length of a typed array, or throws if its buffer is detached, it is
// out of bounds, or its elements wouldn't fit in a 32-bit memory.
static bool GetScalarArrayLength(JSContext* cx, TypedArrayObject* array,
                                 uint32_t* length) {
  mozilla::Maybe<size_t> maybeLength = array->length();
  if (!maybeLength) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TYPED_ARRAY_DETACHED);
    return false;
  }
  if (*maybeLength > INT32_MAX ||
      uint64_t(*maybeLength) * array->bytesPerElement() > UINT32_MAX) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_WASM_COMPONENT_LIST_TOO_LONG);
    return false;
  }
  *length = *maybeLength;
  return true;
}

int32_t wasm::CmScalarArrayLength(Instance* instance, void* valueRef,
                                  int32_t kind) {
  MOZ_ASSERT(SASigCmScalarArrayLength.failureMode == FailureMode::FailOnNegI32);
  uint32_t length;
  if (!GetScalarArrayLength(
          instance->cx(),
          AsScalarArray(AnyRef::fromCompiledCode(valueRef).toJSValue(), kind),
          &length)) {
    return -1;
  }
  return int32_t(length);
}

int32_t wasm::CmCopyScalars(Instance* instance, void* valueRef, int32_t ptr,
                            int32_t length) {
  MOZ_ASSERT(SASigCmCopyScalars.failureMode == FailureMode::FailOnNegI32);
  JSContext* cx = instance->cx();
  TypedArrayObject& array = AnyRef::fromCompiledCode(valueRef)
                                .toJSValue()
                                .toObject()
                                .as<TypedArrayObject>();

  // realloc can run JS that changes the array after its length was read.
  uint32_t currentLength;
  if (!GetScalarArrayLength(cx, &array, &currentLength)) {
    return -1;
  }
  if (currentLength != uint32_t(length)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_WASM_COMPONENT_BAD_LENGTH);
    return -1;
  }
  size_t elemSize = array.bytesPerElement();
  uint64_t byteLength = uint64_t(uint32_t(length)) * elemSize;
  if (!CheckAlignment(instance, uint32_t(ptr), elemSize)) {
    return -1;
  }
  uint8_t* dst = GetMemoryRange(instance, uint32_t(ptr), byteLength);
  if (!dst) {
    return -1;
  }
  // The array could be a view of the memory itself.
  memmove(dst, array.dataPointerEither().unwrap(), byteLength);
  return 0;
}

void* wasm::CmToString(Instance* instance, void* valueRef) {
  MOZ_ASSERT(SASigCmToString.failureMode == FailureMode::FailOnInvalidRef);
  JSContext* cx = instance->cx();
  RootedValue value(cx, AnyRef::fromCompiledCode(valueRef).toJSValue());
  JSString* str = ToString(cx, value);
  if (!str) {
    return AnyRef::invalid().forCompiledCode();
  }
  RootedValue strValue(cx, StringValue(str));
  return ExternRefForCompiledCode(cx, strValue);
}

int32_t wasm::CmStringLength(Instance* instance, void* strRef) {
  MOZ_ASSERT(SASigCmStringLength.failureMode == FailureMode::Infallible);
  return int32_t(
      AnyRef::fromCompiledCode(strRef).toJSValue().toString()->length());
}

static JSLinearString* LinearStringFromRef(JSContext* cx, void* strRef) {
  return AnyRef::fromCompiledCode(strRef).toJSValue().toString()->ensureLinear(
      cx);
}

int32_t wasm::CmStoreASCII(Instance* instance, void* strRef, int32_t ptr,
                           int32_t length) {
  MOZ_ASSERT(SASigCmStoreASCII.failureMode == FailureMode::FailOnNegI32);
  JSLinearString* str = LinearStringFromRef(instance->cx(), strRef);
  if (!str) {
    return -1;
  }
  uint8_t* dst = GetMemoryRange(instance, uint32_t(ptr), uint32_t(length));
  if (!dst) {
    return -1;
  }

  JS::AutoCheckCannotGC nogc;
  size_t i = 0;
  for (; i < str->length(); i++) {
    char16_t c = str->latin1OrTwoByteChar(i);
    if (c >= 0x80) {
      break;
    }
    dst[i] = uint8_t(c);
  }
  return int32_t(i);
}

int32_t wasm::CmStoreUTF8(Instance* instance, void* ctxRef, void* strRef,
                          int32_t ptr, int32_t length) {
  MOZ_ASSERT(SASigCmStoreUTF8.failureMode == FailureMode::FailOnNegI32);
  JSContext* cx = instance->cx();
  RootedObject ctx(cx, &AnyRef::fromCompiledCode(ctxRef).toJSObject());
  JSLinearString* str = LinearStringFromRef(cx, strRef);
  if (!str) {
    return -1;
  }
  uint8_t* dst = GetMemoryRange(instance, uint32_t(ptr), uint32_t(length));
  if (!dst) {
    return -1;
  }

  // This replaces lone surrogates with U+FFFD, as for a USVString. It also
  // rewrites the ASCII prefix that CmStoreASCII already wrote, which is
  // unobservable.
  size_t written = JS::DeflateStringToUTF8Buffer(
      str, mozilla::Span(reinterpret_cast<char*>(dst), uint32_t(length)));
  MOZ_ASSERT(written == JS::GetDeflatedUTF8StringLength(str));
  InstanceFromCtx(ctx).setScratch(written);
  return 0;
}

int32_t wasm::CmStoreUTF16(Instance* instance, void* strRef, int32_t ptr,
                           int32_t length) {
  MOZ_ASSERT(SASigCmStoreUTF16.failureMode == FailureMode::FailOnNegI32);
  JSLinearString* str = LinearStringFromRef(instance->cx(), strRef);
  if (!str || !CheckAlignment(instance, uint32_t(ptr), 2)) {
    return -1;
  }
  uint8_t* dst =
      GetMemoryRange(instance, uint32_t(ptr), 2 * uint64_t(uint32_t(length)));
  if (!dst) {
    return -1;
  }

  // Replace lone surrogates with U+FFFD, as for a USVString.
  JS::AutoCheckCannotGC nogc;
  size_t n = str->length();
  for (size_t i = 0; i < n; i++) {
    char16_t c = str->latin1OrTwoByteChar(i);
    if (unicode::IsLeadSurrogate(c) && i + 1 < n &&
        unicode::IsTrailSurrogate(str->latin1OrTwoByteChar(i + 1))) {
      mozilla::LittleEndian::writeUint16(dst + 2 * i, c);
      i++;
      c = str->latin1OrTwoByteChar(i);
    } else if (unicode::IsSurrogate(c)) {
      c = unicode::REPLACEMENT_CHARACTER;
    }
    mozilla::LittleEndian::writeUint16(dst + 2 * i, c);
  }
  return 0;
}

int32_t wasm::CmStoreLatin1(Instance* instance, void* strRef, int32_t ptr,
                            int32_t length) {
  MOZ_ASSERT(SASigCmStoreLatin1.failureMode == FailureMode::FailOnNegI32);
  JSLinearString* str = LinearStringFromRef(instance->cx(), strRef);
  if (!str || !CheckAlignment(instance, uint32_t(ptr), 2)) {
    return -1;
  }
  uint8_t* dst = GetMemoryRange(instance, uint32_t(ptr), uint32_t(length));
  if (!dst) {
    return -1;
  }

  JS::AutoCheckCannotGC nogc;
  size_t i = 0;
  for (; i < str->length(); i++) {
    char16_t c = str->latin1OrTwoByteChar(i);
    if (c > JSString::MAX_LATIN1_CHAR) {
      break;
    }
    dst[i] = uint8_t(c);
  }
  return int32_t(i);
}

int32_t wasm::CmStoreLatin1ToUTF16(Instance* instance, void* strRef,
                                   int32_t ptr, int32_t latin1Length) {
  MOZ_ASSERT(SASigCmStoreLatin1ToUTF16.failureMode ==
             FailureMode::FailOnNegI32);
  JSLinearString* str = LinearStringFromRef(instance->cx(), strRef);
  if (!str || !CheckAlignment(instance, uint32_t(ptr), 2)) {
    return -1;
  }
  size_t length = str->length();
  uint8_t* dst = GetMemoryRange(instance, uint32_t(ptr), 2 * uint64_t(length));
  if (!dst) {
    return -1;
  }

  // Inflate the Latin-1 prefix, which realloc moved to `ptr`, in place.
  for (int32_t i = latin1Length - 1; i >= 0; i--) {
    dst[2 * i] = dst[i];
    dst[2 * i + 1] = 0;
  }

  // Then store the rest as UTF-16, replacing lone surrogates with U+FFFD as
  // for a USVString.
  JS::AutoCheckCannotGC nogc;
  for (size_t i = latin1Length; i < length; i++) {
    char16_t c = str->latin1OrTwoByteChar(i);
    if (unicode::IsLeadSurrogate(c) && i + 1 < length &&
        unicode::IsTrailSurrogate(str->latin1OrTwoByteChar(i + 1))) {
      mozilla::LittleEndian::writeUint16(dst + 2 * i, c);
      i++;
      c = str->latin1OrTwoByteChar(i);
    } else if (unicode::IsSurrogate(c)) {
      c = unicode::REPLACEMENT_CHARACTER;
    }
    mozilla::LittleEndian::writeUint16(dst + 2 * i, c);
  }
  return 0;
}

// The canonical ABI's load_string_from_range, converted to a JS string.
void* wasm::CmStringToJS(Instance* instance, int32_t ptr, int32_t taggedLength,
                         int32_t encoding) {
  MOZ_ASSERT(SASigCmStringToJS.failureMode == FailureMode::FailOnInvalidRef);
  JSContext* cx = instance->cx();

  uint32_t length = uint32_t(taggedLength);
  bool isLatin1 = false;
  uint32_t alignment = 2;
  switch (ComponentStringEncoding(encoding)) {
    case ComponentStringEncoding::UTF8:
      alignment = 1;
      break;
    case ComponentStringEncoding::UTF16:
      break;
    case ComponentStringEncoding::Latin1PlusUTF16: {
      isLatin1 = !(length & UTF16Tag);
      length &= ~UTF16Tag;
      break;
    }
  }
  bool isUTF8 =
      ComponentStringEncoding(encoding) == ComponentStringEncoding::UTF8;
  uint64_t byteLength = isUTF8 || isLatin1 ? length : 2 * uint64_t(length);

  constexpr uint64_t MaxStringByteLength = (1 << 28) - 1;
  if (byteLength > MaxStringByteLength) {
    ReportTrapError(cx, JSMSG_WASM_COMPONENT_STRING_TOO_LONG);
    return AnyRef::invalid().forCompiledCode();
  }
  if (!CheckAlignment(instance, uint32_t(ptr), alignment)) {
    return AnyRef::invalid().forCompiledCode();
  }
  const uint8_t* src = GetMemoryRange(instance, uint32_t(ptr), byteLength);
  if (!src) {
    return AnyRef::invalid().forCompiledCode();
  }

  // Memory is not moved by GC, so it's safe to read while allocating.
  JSLinearString* str;
  if (isUTF8) {
    auto chars = mozilla::Span(reinterpret_cast<const char*>(src), length);
    if (!mozilla::IsUtf8(chars)) {
      ReportTrapError(cx, JSMSG_WASM_COMPONENT_BAD_STRING);
      return AnyRef::invalid().forCompiledCode();
    }
    str = NewStringCopyUTF8N(cx, JS::UTF8Chars(chars.data(), length));
  } else if (isLatin1) {
    str = NewStringCopyN<CanGC>(cx, reinterpret_cast<const Latin1Char*>(src),
                                length);
  } else {
    Vector<char16_t, 32> chars(cx);
    if (!chars.resize(length)) {
      return AnyRef::invalid().forCompiledCode();
    }
    for (uint32_t i = 0; i < length; i++) {
      chars[i] = mozilla::LittleEndian::readUint16(src + 2 * i);
    }
    for (uint32_t i = 0; i < length; i++) {
      if (unicode::IsLeadSurrogate(chars[i]) && i + 1 < length &&
          unicode::IsTrailSurrogate(chars[i + 1])) {
        i++;
      } else if (unicode::IsSurrogate(chars[i])) {
        ReportTrapError(cx, JSMSG_WASM_COMPONENT_BAD_STRING);
        return AnyRef::invalid().forCompiledCode();
      }
    }
    str = NewStringCopyN<CanGC>(cx, chars.begin(), length);
  }
  if (!str) {
    return AnyRef::invalid().forCompiledCode();
  }
  RootedValue value(cx, StringValue(str));
  return ExternRefForCompiledCode(cx, value);
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
