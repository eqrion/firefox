/*
 * Copyright 2015 Mozilla Foundation
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

#include "wasm/WasmIonCompile.h"

#include "mozilla/DebugOnly.h"

#include <algorithm>

#include "jit/ABIArgGenerator.h"
#include "jit/CodeGenerator.h"
#include "jit/CompileInfo.h"
#include "jit/Ion.h"
#include "jit/IonOptimizationLevels.h"
#include "jit/MIR-wasm.h"
#include "jit/MIR.h"
#include "jit/ShuffleAnalysis.h"
#include "js/GCAPI.h"       // JS::AutoSuppressGCAnalysis
#include "js/ScalarType.h"  // js::Scalar::Type
#include "util/DifferentialTesting.h"
#include "wasm/WasmBaselineCompile.h"
#include "wasm/WasmBuiltinModule.h"
#include "wasm/WasmBuiltins.h"
#include "wasm/WasmCodegenTypes.h"
#include "wasm/WasmGC.h"
#include "wasm/WasmGcObject.h"
#include "wasm/WasmGenerator.h"
#include "wasm/WasmOpIter.h"
#include "wasm/WasmSignalHandlers.h"
#include "wasm/WasmStacks.h"
#include "wasm/WasmStubs.h"
#include "wasm/WasmValidate.h"
#include "wasm/WasmItasca.h"
#include "wasm/WasmItascaOps.h"

using namespace js;
using namespace js::jit;
using namespace js::wasm;

using mozilla::Nothing;

// Forward-declared so FunctionCompiler can friend it: the itasca hooks call
// straight into FunctionCompiler's mostly-private emitters, and are defined
// well below it, at file scope rather than in FunctionCompiler's own unnamed
// namespace.
struct IonItascaHooks;

namespace {

using UniqueCompileInfo = UniquePtr<CompileInfo>;
using UniqueCompileInfoVector = Vector<UniqueCompileInfo, 1, SystemAllocPolicy>;

using BlockVector = Vector<MBasicBlock*, 8, SystemAllocPolicy>;
using DefVector = Vector<MDefinition*, 8, SystemAllocPolicy>;
using ControlInstructionVector =
    Vector<MControlInstruction*, 8, SystemAllocPolicy>;

// [SMDOC] WebAssembly Exception Handling in Ion
// =======================================================
//
// ## Throwing instructions
//
// Wasm exceptions can be thrown by either a throw instruction (local throw),
// or by a wasm call.
//
// ## The "catching try control"
//
// We know we are in try-code if there is a surrounding ControlItem with
// LabelKind::Try. The innermost such control is called the
// "catching try control".
//
// ## Throws without a catching try control
//
// Such throws are implemented with an instance call that triggers the exception
// unwinding runtime. The exception unwinding runtime will not return to the
// function.
//
// ## "landing pad" and "pre-pad" blocks
//
// When an exception is thrown, the unwinder will search for the nearest
// enclosing try block and redirect control flow to it. The code that executes
// before any catch blocks is called the 'landing pad'. The 'landing pad' is
// responsible to:
//   1. Consume the pending exception state from
//      Instance::pendingException(Tag)
//   2. Branch to the correct catch block, or else rethrow
//
// There is one landing pad for each try block. The immediate predecessors of
// the landing pad are called 'pre-pad' blocks. There is one pre-pad block per
// throwing instruction.
//
// ## Creating pre-pad blocks
//
// There are two possible sorts of pre-pad blocks, depending on whether we
// are branching after a local throw instruction, or after a wasm call:
//
// - If we encounter a local throw, we create the exception and tag objects,
//   store them to Instance::pendingException(Tag), and then jump to the
//   landing pad.
//
// - If we encounter a wasm call, we construct a MWasmCallCatchable which is a
//   control instruction with either a branch to a fallthrough block or
//   to a pre-pad block.
//
//   The pre-pad block for a wasm call is empty except for a jump to the
//   landing pad. It only exists to avoid critical edges which when split would
//   violate the invariants of MWasmCallCatchable. The pending exception state
//   is taken care of by the unwinder.
//
// Each pre-pad ends with a pending jump to the landing pad. The pending jumps
// to the landing pad are tracked in `tryPadPatches`. These are called
// "pad patches".
//
// ## Creating the landing pad
//
// When we exit try-code, we check if tryPadPatches has captured any control
// instructions (pad patches). If not, we don't compile any catches and we mark
// the rest as dead code.
//
// If there are pre-pad blocks, we join them to create a landing pad (or just
// "pad"). The pad's last two slots are the caught exception, and the
// exception's tag object.
//
// There are three different forms of try-catch/catch_all Wasm instructions,
// which result in different form of landing pad.
//
// 1. A catchless try, so a Wasm instruction of the form "try ... end".
//    - In this case, we end the pad by rethrowing the caught exception.
//
// 2. A single catch_all after a try.
//    - If the first catch after a try is a catch_all, then there won't be
//      any more catches, but we need the exception and its tag object, in
//      case the code in a catch_all contains "rethrow" instructions.
//      - The Wasm instruction "rethrow", gets the exception and tag object to
//        rethrow from the last two slots of the landing pad which, due to
//        validation, is the l'th surrounding ControlItem.
//      - We immediately GoTo to a new block after the pad and pop both the
//        exception and tag object, as we don't need them anymore in this case.
//
// 3. Otherwise, there is one or more catch code blocks following.
//    - In this case, we construct the landing pad by creating a sequence
//      of compare and branch blocks that compare the pending exception tag
//      object to the tag object of the current tagged catch block. This is
//      done incrementally as we visit each tagged catch block in the bytecode
//      stream. At every step, we update the ControlItem's block to point to
//      the next block to be created in the landing pad sequence. The final
//      block will either be a rethrow, if there is no catch_all, or else a
//      jump to a catch_all block.

struct TryControl {
  // Branches to bind to the try's landing pad.
  ControlInstructionVector landingPadPatches;
  // For `try_table`, the list of tagged catches and labels to branch to.
  TryTableCatchVector catches;
  // The pending exception for the try's landing pad.
  MDefinition* pendingException;
  // The pending exception's tag for the try's landing pad.
  MDefinition* pendingExceptionTag;
  // Whether this try is in the body and should catch any thrown exception.
  bool inBody;

  TryControl()
      : pendingException(nullptr),
        pendingExceptionTag(nullptr),
        inBody(false) {}

  // Reset the try control for when it is cached in FunctionCompiler.
  void reset() {
    landingPadPatches.clearAndFree();
    catches.clearAndFree();
    inBody = false;
  }
};
using UniqueTryControl = UniquePtr<TryControl>;
using VectorUniqueTryControl = Vector<UniqueTryControl, 2, SystemAllocPolicy>;

struct ControlFlowPatch {
  MControlInstruction* ins;
  uint32_t index;
  ControlFlowPatch(MControlInstruction* ins, uint32_t index)
      : ins(ins), index(index) {}
};

using ControlFlowPatchVector = Vector<ControlFlowPatch, 0, SystemAllocPolicy>;

struct PendingBlockTarget {
  ControlFlowPatchVector patches;
  BranchHint hint = BranchHint::Invalid;
};

using PendingBlockTargetVector =
    Vector<PendingBlockTarget, 0, SystemAllocPolicy>;

// Inlined functions accumulate all returns to be bound to a caller function
// after compilation is finished.
struct PendingInlineReturn {
  PendingInlineReturn(MGoto* jump, DefVector&& results)
      : jump(jump), results(std::move(results)) {}

  MGoto* jump;
  DefVector results;
};

using PendingInlineReturnVector =
    Vector<PendingInlineReturn, 1, SystemAllocPolicy>;

// CallCompileState describes a call that is being compiled.
struct CallCompileState {
  // A generator object that is passed each argument as it is compiled.
  ABIArgGenerator abi;

  // Whether we pass FP values through GPRs or FPRs.
  bool hardFP = true;

  // The ABI we are using for this call.
  ABIKind abiKind;

  // Accumulates the register arguments while compiling arguments.
  MWasmCallBase::Args regArgs;

  // Reserved argument for passing Instance* to builtin instance method calls.
  ABIArg instanceArg;

  // The stack area in which the callee will write stack return values, or
  // nullptr if no stack results.
  MWasmStackResultArea* stackResultArea = nullptr;

  // Indicates that the call is a return/tail call.
  bool returnCall = false;

  // The landing pad patches for the nearest enclosing try-catch. This is
  // non-null iff the call is catchable.
  ControlInstructionVector* tryLandingPadPatches = nullptr;

  // The index of the try note for a catchable call.
  uint32_t tryNoteIndex = UINT32_MAX;

  // The block to take for fallthrough execution for a catchable call.
  MBasicBlock* fallthroughBlock = nullptr;

  // The block to take for exceptional execution for a catchable call.
  MBasicBlock* prePadBlock = nullptr;

  explicit CallCompileState(ABIKind abiKind) : abi(abiKind), abiKind(abiKind) {
    if (abiKind == ABIKind::System) {
      // The system ABI follows the hardFP setting on ARM32.
#if defined(JS_CODEGEN_ARM)
      hardFP = ARMFlags::UseHardFpABI();
      abi.setUseHardFp(hardFP);
#endif
    } else {
#if defined(JS_CODEGEN_ARM)
      MOZ_ASSERT(hardFP, "The WASM ABI passes FP arguments in registers");
#endif
    }
  }

  bool isCatchable() const { return tryLandingPadPatches != nullptr; }
};

struct Control {
  MBasicBlock* block;
  UniqueTryControl tryControl;

  Control() : block(nullptr), tryControl(nullptr) {}
  Control(Control&&) = default;
  Control(const Control&) = delete;
};

// Statistics for inlining (at all depths) into the root function.
struct InliningStats {
  size_t inlinedDirectBytecodeSize = 0;   // sum of sizes of inlinees
  size_t inlinedDirectFunctions = 0;      // number of inlinees
  size_t inlinedCallRefBytecodeSize = 0;  // sum of sizes of inlinees
  size_t inlinedCallRefFunctions = 0;     // number of inlinees
  bool largeFunctionBackoff = false;      // did large function backoff happen?
};

// Encapsulates the generation of MIR for a wasm function and any functions
// that become inlined into it.
class RootCompiler {
  const CompilerEnvironment& compilerEnv_;
  const CodeMetadata& codeMeta_;
  const CodeTailMetadata* codeTailMeta_;

  const FuncCompileInput& func_;
  Decoder& decoder_;
  FeatureUsage observedFeatures_;

  CompileInfo compileInfo_;
  const JitCompileOptions options_;
  TempAllocator& alloc_;
  MIRGraph mirGraph_;
  MIRGenerator mirGen_;

  // The current loop depth we're generating inside of. This includes all
  // callee functions when we're generating an inlined function, and so it
  // lives here on the root compiler.
  uint32_t loopDepth_;

  // The current stack of bytecode offsets of the caller functions of the
  // function currently being inlined.
  BytecodeOffsetVector inlinedCallerOffsets_;
  InlinedCallerOffsetIndex inlinedCallerOffsetsIndex_;

  // Compilation statistics for this function.
  CompileStats funcStats_;

  // Accumulated inlining statistics for this function.
  InliningStats inliningStats_;
  // The remaining inlining budget, in terms of bytecode bytes. This may go
  // negative and so is signed.
  int64_t localInliningBudget_;

  // All jit::CompileInfo objects created during this compilation. This must
  // be kept alive for as long as the MIR graph is alive.
  UniqueCompileInfoVector compileInfos_;

  // Cache of TryControl to minimize heap allocations.
  VectorUniqueTryControl tryControlCache_;

  // Reference to masm.tryNotes()
  wasm::TryNoteVector& tryNotes_;

  // Reference to masm.inliningContext()
  wasm::InliningContext& inliningContext_;

 public:
  RootCompiler(const CompilerEnvironment& compilerEnv,
               const CodeMetadata& codeMeta,
               const CodeTailMetadata* codeTailMeta, TempAllocator& alloc,
               const FuncCompileInput& func, Decoder& decoder,
               wasm::TryNoteVector& tryNotes,
               wasm::InliningContext& inliningContext,
               const BytecodeRegion& codeBytes, const VwEnv* itascaEnv,
               UniqueChars* error, VwBatch* validationBatch)
      : compilerEnv_(compilerEnv),
        codeMeta_(codeMeta),
        codeTailMeta_(codeTailMeta),
        func_(func),
        decoder_(decoder),
        observedFeatures_(FeatureUsage::None),
        // The real count arrives later, from on_function_start's VwContext;
        // setWasmNumLocals fixes this up before any local is referenced.
        compileInfo_(0),
        alloc_(alloc),
        mirGraph_(&alloc),
        mirGen_(nullptr, options_, &alloc_, &mirGraph_, &compileInfo_,
                IonOptimizations.get(OptimizationLevel::Wasm), &codeMeta),
        loopDepth_(0),
        localInliningBudget_(0),
        tryNotes_(tryNotes),
        inliningContext_(inliningContext),
        codeBytes_(codeBytes),
        itascaEnv_(itascaEnv),
        validationBatch_(validationBatch),
        error_(error) {}

  // The code section and the environment its bodies are validated against.
  const BytecodeRegion codeBytes_;
  const VwEnv* const itascaEnv_;
  VwBatch* const validationBatch_;
  UniqueChars* const error_;

  const CompilerEnvironment& compilerEnv() const { return compilerEnv_; }
  const CodeMetadata& codeMeta() const { return codeMeta_; }
  const CodeTailMetadata* codeTailMeta() const { return codeTailMeta_; }
  const FuncCompileInput& func() const { return func_; }
  // Called from on_function_start, once the validator has decoded the
  // function's declared locals.
  void setWasmNumLocals(uint32_t numLocals) {
    compileInfo_.setWasmNumLocals(numLocals);
  }
  TempAllocator& alloc() { return alloc_; }
  MIRGraph& mirGraph() { return mirGraph_; }
  MIRGenerator& mirGen() { return mirGen_; }
  int64_t inliningBudget() const { return localInliningBudget_; }
  FeatureUsage observedFeatures() const { return observedFeatures_; }
  const CompileStats& funcStats() const { return funcStats_; }
  void noteLargeFunctionBackoffWasApplied() {
    inliningStats_.largeFunctionBackoff = true;
  }

  uint32_t loopDepth() const { return loopDepth_; }
  void startLoop() { loopDepth_++; }
  void closeLoop() { loopDepth_--; }

  [[nodiscard]] bool generate();

  InlinedCallerOffsetIndex inlinedCallerOffsetsIndex() const {
    return inlinedCallerOffsetsIndex_;
  }

  // Add a compile info for an inlined function. This keeps the inlined
  // function's compile info alive for the outermost function's
  // compilation.
  [[nodiscard]] CompileInfo* startInlineCall(
      uint32_t callerFuncIndex, BytecodeOffset callerOffset,
      uint32_t calleeFuncIndex, uint32_t numLocals, size_t inlineeBytecodeSize,
      InliningHeuristics::CallKind callKind);
  void finishInlineCall();

  // Add a try note and return the index.
  [[nodiscard]] bool addTryNote(uint32_t* tryNoteIndex) {
    if (!tryNotes_.append(wasm::TryNote())) {
      return false;
    }
    *tryNoteIndex = tryNotes_.length() - 1;
    return true;
  }

  // Try to get a free TryControl from the cache, or allocate a new one.
  [[nodiscard]] UniqueTryControl newTryControl() {
    if (tryControlCache_.empty()) {
      return UniqueTryControl(js_new<TryControl>());
    }
    UniqueTryControl tryControl = std::move(tryControlCache_.back());
    tryControlCache_.popBack();
    return tryControl;
  }

  // Release the TryControl to the cache.
  void freeTryControl(UniqueTryControl&& tryControl) {
    // Ensure that it's in a consistent state
    tryControl->reset();
    // Ignore any OOM, as we'll fail later
    (void)tryControlCache_.append(std::move(tryControl));
  }
};

// Encapsulates the generation of MIR for a single function in a wasm module.
class FunctionCompiler {
  // The itasca hooks call straight into the emitter methods below, most of
  // which are private.
  friend struct ::IonItascaHooks;

  // The root function compiler we are being compiled within.
  RootCompiler& rootCompiler_;

  // The caller function compiler, if any, that we are being inlined into.
  // Note that `inliningDepth_` is zero for the first inlinee, one for the
  // second inlinee, etc.
  const FunctionCompiler* callerCompiler_;
  const uint32_t inliningDepth_;

  // Decoder for this function, used for misc error reporting: nothing reads
  // bytes through it any more, the validator does the reading.
  Decoder& decoder_;

  // The byte offset of the operator currently being emitted, within the
  // function body's own bytes. There is no readOp to record this, since the
  // validator read the operator; hooks hand it over instead.
  size_t currentOpOffset_ = 0;

  // Ion's own operand stack, replacing OpIter's: every entry is a real
  // MDefinition*, pushed and popped by the emitters themselves rather than
  // through a validator-shaped read/push pair.
  DefVector valueStack_;

  // The control stack, replacing OpIter's. Each frame's Control is Ion's own
  // payload (the block's MBasicBlock*, and try state, unused since wasm 1.0
  // has no exceptions); its LabelKind and BlockType are what a branch to it,
  // or the `end` that closes it, need; valueStackBase is where this frame's
  // slice of valueStack_ starts, and polymorphicBase says whether this block
  // is already unreachable, so pops past that point synthesize a null
  // MDefinition* (wasm's unreachable code has values of no fixed type) rather
  // than underflow.
  struct ControlFrame {
    Control item;
    LabelKind kind;
    BlockType type;
    uint32_t valueStackBase;
    bool polymorphicBase = false;
    ControlFrame(LabelKind kind, BlockType type, uint32_t valueStackBase)
        : kind(kind), type(type), valueStackBase(valueStackBase) {}
  };
  Vector<ControlFrame, 16, SystemAllocPolicy> controlStack_;

  uint32_t functionBodyOffset_;
  const FuncCompileInput& func_;
  // Set by init(), once on_function_start has decoded the function's
  // locals out of the validator's VwContext: nothing needs them before
  // then, and nothing after this constructs a FunctionCompiler with real
  // locals in hand.
  const ValTypeVector* locals_ = nullptr;
  size_t numCallRefs_;
  size_t numAllocSites_;

  // CompileInfo for compiling the MIR for this function. Allocated inside of
  // RootCompiler::compileInfos, and kept alive for the duration of the
  // total compilation.
  const jit::CompileInfo& info_;

  MBasicBlock* curBlock_;
  uint32_t maxStackArgBytes_;

  // When generating a forward branch we haven't created the basic block that
  // the branch needs to target. We handle this by accumulating all the branch
  // instructions that want to target a block we have not yet created into
  // `pendingBlocks_` and then patching them in `bindBranches`.
  //
  // For performance reasons we only grow `pendingBlocks_` as needed, never
  // shrink it. So the length of the vector has no relation to the current
  // nesting depth of wasm blocks. We use `pendingBlockDepth_` to track the
  // current wasm block depth. We assert that all entries beyond the current
  // block depth are empty.
  uint32_t pendingBlockDepth_;
  PendingBlockTargetVector pendingBlocks_;
  // Control flow patches for exceptions that are caught without a landing
  // pad they can directly jump to. This happens when either:
  //  (1) `delegate` targets the function body label.
  //  (2) A `try` ends without any cases, and there is no enclosing `try`.
  //  (3) There is no `try` in this function, but a caller function (when
  //      inlining) has a `try`.
  //
  // These exceptions will be rethrown using `emitBodyRethrowPad`.
  ControlInstructionVector bodyRethrowPadPatches_;
  // A vector of the returns in this function for use when we're being inlined
  // into another function.
  PendingInlineReturnVector pendingInlineReturns_;
  // A block that all uncaught exceptions in this function will jump to. The
  // inline caller will link this to the nearest enclosing catch handler.
  MBasicBlock* pendingInlineCatchBlock_;

  // Instance pointer argument to the current function.
  MWasmParameter* instancePointer_;
  MWasmParameter* stackResultPointer_;

 public:
  // Construct a FunctionCompiler for the root function of a compilation.
  // Its locals are not known yet; init() sets them once on_function_start
  // has decoded them.
  FunctionCompiler(RootCompiler& rootCompiler, Decoder& decoder,
                   const FuncCompileInput& func, const CompileInfo& compileInfo)
      : rootCompiler_(rootCompiler),
        callerCompiler_(nullptr),
        inliningDepth_(0),
        decoder_(decoder),
        functionBodyOffset_(decoder.beginOffset()),
        func_(func),
        numCallRefs_(0),
        numAllocSites_(0),
        info_(compileInfo),
        curBlock_(nullptr),
        maxStackArgBytes_(0),
        pendingBlockDepth_(0),
        pendingInlineCatchBlock_(nullptr),
        instancePointer_(nullptr),
        stackResultPointer_(nullptr) {}

  // Construct a FunctionCompiler for an inlined callee of a compilation
  FunctionCompiler(const FunctionCompiler* callerCompiler, Decoder& decoder,
                   const FuncCompileInput& func, const ValTypeVector& locals,
                   const CompileInfo& compileInfo)
      : rootCompiler_(callerCompiler->rootCompiler_),
        callerCompiler_(callerCompiler),
        inliningDepth_(callerCompiler_->inliningDepth() + 1),
        decoder_(decoder),
        functionBodyOffset_(decoder.beginOffset()),
        func_(func),
        locals_(&locals),
        numCallRefs_(0),
        numAllocSites_(0),
        info_(compileInfo),
        curBlock_(nullptr),
        maxStackArgBytes_(0),
        pendingBlockDepth_(0),
        pendingInlineCatchBlock_(nullptr),
        instancePointer_(callerCompiler_->instancePointer_),
        stackResultPointer_(nullptr) {}

  RootCompiler& rootCompiler() { return rootCompiler_; }
  const CodeMetadata& codeMeta() const { return rootCompiler_.codeMeta(); }
  const CodeTailMetadata* codeTailMeta() const {
    return rootCompiler_.codeTailMeta();
  }

  // Binds the root function's locals, decoded by on_function_start out of
  // the validator's VwContext. `locals` must outlive this FunctionCompiler.
  void init(const ValTypeVector& locals) { locals_ = &locals; }

  // --- Ion's own operand and control stack, replacing OpIter's ---

  [[nodiscard]] bool push(MDefinition* def) { return valueStack_.append(def); }
  [[nodiscard]] bool pushN(const DefVector& defs) {
    return valueStack_.append(defs.begin(), defs.length());
  }

  // Pop one value. In already-unreachable code this may run out of real
  // values to pop; wasm's unreachable code has values of no fixed type, so a
  // null MDefinition* stands in rather than underflowing.
  MDefinition* pop() {
    ControlFrame& top = controlStack_.back();
    MOZ_ASSERT(valueStack_.length() >= top.valueStackBase);
    if (valueStack_.length() == top.valueStackBase) {
      MOZ_ASSERT(top.polymorphicBase);
      return nullptr;
    }
    MDefinition* def = valueStack_.back();
    valueStack_.popBack();
    return def;
  }

  [[nodiscard]] bool popN(size_t n, DefVector* out) {
    if (!out->resize(n)) {
      return false;
    }
    for (size_t i = 0; i < n; i++) {
      (*out)[n - 1 - i] = pop();
    }
    return true;
  }

  void popBinary(MDefinition** lhs, MDefinition** rhs) {
    *rhs = pop();
    *lhs = pop();
  }

  // Read, but do not remove, the top `n` values: what a branch carries to its
  // target, which (for a conditional branch) are still there for the
  // fallthrough path afterward. Pads with null past the polymorphic base the
  // same way pop() does - and, unlike pop(), also inserts that padding into
  // valueStack_ itself, since a pokeN() overwriting a control join's result
  // needs a real slot there to overwrite, whether or not this path actually
  // produced one.
  [[nodiscard]] bool peekN(size_t n, DefVector* out) {
    ControlFrame& top = controlStack_.back();
    MOZ_ASSERT(valueStack_.length() >= top.valueStackBase);
    size_t avail =
        std::min<size_t>(n, valueStack_.length() - top.valueStackBase);
    size_t pad = n - avail;
    if (!out->resize(n)) {
      return false;
    }
    for (size_t i = 0; i < pad; i++) {
      (*out)[i] = nullptr;
      if (!valueStack_.insert(valueStack_.begin() + top.valueStackBase,
                              nullptr)) {
        return false;
      }
    }
    for (size_t i = 0; i < avail; i++) {
      (*out)[pad + i] = valueStack_[valueStack_.length() - avail + i];
    }
    return true;
  }

  // Overwrite the top `n` values in place: what a control join does, after
  // building the real (phi-merged) values that replace whatever a single
  // incoming path had peeked there.
  void pokeN(size_t n, const DefVector& values) {
    MOZ_ASSERT(valueStack_.length() >= n);
    MOZ_ASSERT(values.length() == n);
    for (size_t i = 0; i < n; i++) {
      valueStack_[valueStack_.length() - n + i] = values[i];
    }
  }

  // An unconditional branch (br, br_table, and the fallthrough at a body's
  // end) makes the rest of the current block unreachable: reset the value
  // stack to this block's own base and mark it polymorphic, so any further
  // pop before the matching `end` pads with null instead of underflowing.
  void afterUnconditionalBranch() {
    ControlFrame& top = controlStack_.back();
    valueStack_.shrinkTo(top.valueStackBase);
    top.polymorphicBase = true;
  }

  [[nodiscard]] bool pushControl(LabelKind kind, BlockType type) {
    return controlStack_.emplaceBack(kind, type, valueStack_.length());
  }

  Control& controlItem() { return controlStack_.back().item; }
  Control& controlItem(uint32_t relativeDepth) {
    return controlStack_[controlStack_.length() - 1 - relativeDepth].item;
  }
  LabelKind controlKind(uint32_t relativeDepth) {
    return controlStack_[controlStack_.length() - 1 - relativeDepth].kind;
  }
  size_t controlStackDepth() const { return controlStack_.length(); }
  bool controlStackEmpty() const { return controlStack_.empty(); }

  // A linear scan for the innermost frame at or below `fromRelativeDepth`
  // whose (kind, Control) satisfies `predicate`. Only try/try_table's search
  // for an enclosing try uses this, and wasm 1.0 has neither, but the search
  // itself is cheap and correct regardless.
  template <typename Predicate>
  bool controlFindInnermostFrom(Predicate predicate, uint32_t fromRelativeDepth,
                                uint32_t* foundRelativeDepth) const {
    int32_t fromAbsoluteDepth = controlStack_.length() - fromRelativeDepth - 1;
    for (int32_t i = fromAbsoluteDepth; i >= 0; i--) {
      if (predicate(controlStack_[i].kind, controlStack_[i].item)) {
        *foundRelativeDepth = controlStack_.length() - 1 - i;
        return true;
      }
    }
    return false;
  }

  // What a branch to this depth carries: a loop's parameters (branching to a
  // loop targets its header, which is not a control join, so nothing joins
  // there) or every other kind's results.
  ResultType branchTargetType(uint32_t relativeDepth) {
    const ControlFrame& frame =
        controlStack_[controlStack_.length() - 1 - relativeDepth];
    return frame.kind == LabelKind::Loop ? frame.type.params()
                                         : frame.type.results();
  }
  uint32_t relativeBytecodeOffset() {
    return readBytecodeOffset() - functionBodyOffset_;
  }
  TempAllocator& alloc() const { return rootCompiler_.alloc(); }
  // FIXME(1401675): Replace with BlockType.
  uint32_t funcIndex() const { return func_.index; }
  const FuncType& funcType() const {
    return codeMeta().getFuncType(func_.index);
  }

  bool isInlined() const { return callerCompiler_ != nullptr; }
  uint32_t inliningDepth() const { return inliningDepth_; }

  MBasicBlock* getCurBlock() const { return curBlock_; }
  BytecodeOffset bytecodeOffset() const {
    return BytecodeOffset(currentOpOffset_);
  }
  CallSiteDesc callSiteDesc(CallSiteKind kind) {
    return CallSiteDesc(bytecodeOffset().offset(),
                        rootCompiler_.inlinedCallerOffsetsIndex(), kind);
  }
  TrapSiteDesc trapSiteDesc() {
    return TrapSiteDesc(wasm::BytecodeOffset(bytecodeOffset()),
                        rootCompiler_.inlinedCallerOffsetsIndex());
  }
  TrapSiteDesc trapSiteDescWithCallSiteLineNumber() {
    return TrapSiteDesc(wasm::BytecodeOffset(readBytecodeOffset()),
                        rootCompiler_.inlinedCallerOffsetsIndex());
  }
  // Only ever set by opcodes wasm 1.0's hooks cannot reach.
  FeatureUsage featureUsage() const { return FeatureUsage::None; }

  [[nodiscard]] bool initRoot() {
    // We are not being inlined into something
    MOZ_ASSERT(!callerCompiler_);

    // Prepare the entry block for MIR generation:

    const FuncType& ft = funcType();
    const ArgTypeVector args(ft);

    if (!mirGen().ensureBallast()) {
      return false;
    }
    if (!newBlock(/* prev */ nullptr, &curBlock_)) {
      return false;
    }

    for (ABIArgIter i(args, ABIKind::Wasm); !i.done(); i++) {
      MaybeRefType argRefType;
      if (!args.isSyntheticStackResultPointerArg(i.index())) {
        ValType argType = ft.arg(i.index());
        argRefType = argType.isRefType() ? MaybeRefType(argType.refType())
                                         : MaybeRefType();
      }

      MWasmParameter* ins =
          MWasmParameter::New(alloc(), *i, i.mirType(), argRefType);
      curBlock_->add(ins);
      if (args.isSyntheticStackResultPointerArg(i.index())) {
        MOZ_ASSERT(stackResultPointer_ == nullptr);
        stackResultPointer_ = ins;
      } else {
        curBlock_->initSlot(info().localSlot(args.naturalIndex(i.index())),
                            ins);
      }
      if (!mirGen().ensureBallast()) {
        return false;
      }
    }

    // Set up a parameter that receives the hidden instance pointer argument.
    instancePointer_ =
        MWasmParameter::New(alloc(), ABIArg(InstanceReg), MIRType::Pointer);
    curBlock_->add(instancePointer_);
    if (!mirGen().ensureBallast()) {
      return false;
    }

    for (size_t i = args.lengthWithoutStackResults(); i < locals_->length();
         i++) {
      ValType slotValType = (*locals_)[i];
#ifndef ENABLE_WASM_SIMD
      if (slotValType == ValType::V128) {
        return fail("Ion has no SIMD support yet");
      }
#endif
      MDefinition* zero = constantZeroOfValType(slotValType);
      curBlock_->initSlot(info().localSlot(i), zero);
      if (!mirGen().ensureBallast()) {
        return false;
      }
    }

    return true;
  }

  [[nodiscard]] bool initInline(const DefVector& argValues) {
    // "This is an inlined-callee FunctionCompiler"
    MOZ_ASSERT(callerCompiler_);

    // Prepare the entry block for MIR generation:
    if (!mirGen().ensureBallast()) {
      return false;
    }
    if (!newBlock(nullptr, &curBlock_)) {
      return false;
    }

    MBasicBlock* pred = callerCompiler_->curBlock_;
    pred->end(MGoto::New(alloc(), curBlock_));
    if (!curBlock_->addPredecessorWithoutPhis(pred)) {
      return false;
    }

    // Set up args slots to point to passed argument values
    const FuncType& type = funcType();
    for (uint32_t argIndex = 0; argIndex < type.args().length(); argIndex++) {
      curBlock_->initSlot(info().localSlot(argIndex), argValues[argIndex]);
    }

    // Set up a parameter that receives the hidden instance pointer argument.
    instancePointer_ = callerCompiler_->instancePointer_;

    // Initialize all local slots to zero value
    for (size_t i = type.args().length(); i < locals_->length(); i++) {
      ValType slotValType = (*locals_)[i];
#ifndef ENABLE_WASM_SIMD
      if (slotValType == ValType::V128) {
        return fail("Ion has no SIMD support yet");
      }
#endif
      MDefinition* zero = constantZeroOfValType(slotValType);
      curBlock_->initSlot(info().localSlot(i), zero);
      if (!mirGen().ensureBallast()) {
        return false;
      }
    }

    return true;
  }

  void finish() {
    mirGen().accumulateWasmMaxStackArgBytes(maxStackArgBytes_);

    MOZ_ASSERT(pendingBlockDepth_ == 0);
#ifdef DEBUG
    for (PendingBlockTarget& targets : pendingBlocks_) {
      MOZ_ASSERT(targets.patches.empty());
    }
#endif
    MOZ_ASSERT(inDeadCode());
    // The body is finished when its outermost block closed, which is what the
    // validator decided. Whether a decoder reached the end is no longer
    // meaningful: nothing reads bytes through it.
    MOZ_ASSERT(controlStackEmpty());
    MOZ_ASSERT_IF(
        compilerEnv().mode() == CompileMode::LazyTiering,
        codeTailMeta()->getFuncDefCallRefs(funcIndex()).length == numCallRefs_);
    MOZ_ASSERT_IF(codeTailMeta(),
                  codeTailMeta()->getFuncDefAllocSites(funcIndex()).length ==
                      numAllocSites_);
    MOZ_ASSERT_IF(!isInlined(),
                  pendingInlineReturns_.empty() && !pendingInlineCatchBlock_);
    MOZ_ASSERT(bodyRethrowPadPatches_.empty());
  }

  /************************* Read-only interface (after local scope setup) */

  MIRGenerator& mirGen() const { return rootCompiler_.mirGen(); }
  MIRGraph& mirGraph() const { return rootCompiler_.mirGraph(); }
  const CompileInfo& info() const { return info_; }
  const CompilerEnvironment& compilerEnv() const {
    return rootCompiler_.compilerEnv();
  }

  MDefinition* getLocalDef(unsigned slot) {
    if (inDeadCode()) {
      return nullptr;
    }
    return curBlock_->getSlot(info().localSlot(slot));
  }

  const ValTypeVector& locals() const { return *locals_; }

  /*********************************************************** Constants ***/

  MDefinition* constantF32(float f) {
    if (inDeadCode()) {
      return nullptr;
    }
    auto* cst = MWasmFloatConstant::NewFloat32(alloc(), f);
    curBlock_->add(cst);
    return cst;
  }
  // Hide all other overloads, to guarantee no implicit argument conversion.
  template <typename T>
  MDefinition* constantF32(T) = delete;

  MDefinition* constantF64(double d) {
    if (inDeadCode()) {
      return nullptr;
    }
    auto* cst = MWasmFloatConstant::NewDouble(alloc(), d);
    curBlock_->add(cst);
    return cst;
  }
  template <typename T>
  MDefinition* constantF64(T) = delete;

  MDefinition* constantI32(int32_t i) {
    if (inDeadCode()) {
      return nullptr;
    }
    MConstant* constant = MConstant::NewInt32(alloc(), i);
    curBlock_->add(constant);
    return constant;
  }
  template <typename T>
  MDefinition* constantI32(T) = delete;

  MDefinition* constantI64(int64_t i) {
    if (inDeadCode()) {
      return nullptr;
    }
    MConstant* constant = MConstant::NewInt64(alloc(), i);
    curBlock_->add(constant);
    return constant;
  }
  template <typename T>
  MDefinition* constantI64(T) = delete;

  // Produce an MConstant of the machine's target int type (Int32 or Int64).
  MDefinition* constantTargetWord(intptr_t n) {
    return targetIs64Bit() ? constantI64(int64_t(n)) : constantI32(int32_t(n));
  }
  template <typename T>
  MDefinition* constantTargetWord(T) = delete;

#ifdef ENABLE_WASM_SIMD
  MDefinition* constantV128(V128 v) {
    if (inDeadCode()) {
      return nullptr;
    }
    MWasmFloatConstant* constant = MWasmFloatConstant::NewSimd128(
        alloc(), SimdConstant::CreateSimd128((int8_t*)v.bytes));
    curBlock_->add(constant);
    return constant;
  }
  template <typename T>
  MDefinition* constantV128(T) = delete;
#endif

  MDefinition* constantNullRef(MaybeRefType type) {
    if (inDeadCode()) {
      return nullptr;
    }
    // MConstant has a lot of baggage so we don't use that here.
    MWasmNullConstant* constant = MWasmNullConstant::New(alloc(), type);
    curBlock_->add(constant);
    return constant;
  }

  // Produce a zero constant for the specified ValType.
  MDefinition* constantZeroOfValType(ValType valType) {
    switch (valType.kind()) {
      case ValType::I32:
        return constantI32(0);
      case ValType::I64:
        return constantI64(int64_t(0));
#ifdef ENABLE_WASM_SIMD
      case ValType::V128:
        return constantV128(V128(0));
#endif
      case ValType::F32:
        return constantF32(0.0f);
      case ValType::F64:
        return constantF64(0.0);
      case ValType::Ref:
        return constantNullRef(MaybeRefType(valType.refType()));
      default:
        MOZ_CRASH();
    }
  }

  /***************************** Code generation (after local scope setup) */

  void fence() {
    if (inDeadCode()) {
      return;
    }
    MWasmFence* ins = MWasmFence::New(alloc());
    curBlock_->add(ins);
  }

  template <class T>
  MDefinition* unary(MDefinition* op) {
    if (inDeadCode()) {
      return nullptr;
    }
    T* ins = T::New(alloc(), op);
    curBlock_->add(ins);
    return ins;
  }

  template <class T>
  MDefinition* unary(MDefinition* op, MIRType type) {
    if (inDeadCode()) {
      return nullptr;
    }
    T* ins = T::New(alloc(), op, type);
    curBlock_->add(ins);
    return ins;
  }

  template <class T>
  MDefinition* binary(MDefinition* lhs, MDefinition* rhs, MIRType type) {
    if (inDeadCode()) {
      return nullptr;
    }
    T* ins = T::New(alloc(), lhs, rhs, type);
    curBlock_->add(ins);
    return ins;
  }

  template <class T>
  MDefinition* binary(MDefinition* lhs, MDefinition* rhs, MIRType type,
                      MWasmBinaryBitwise::SubOpcode subOpc) {
    if (inDeadCode()) {
      return nullptr;
    }
    T* ins = T::New(alloc(), lhs, rhs, type, subOpc);
    curBlock_->add(ins);
    return ins;
  }

  MDefinition* ursh(MDefinition* lhs, MDefinition* rhs, MIRType type) {
    if (inDeadCode()) {
      return nullptr;
    }
    auto* ins = MUrsh::NewWasm(alloc(), lhs, rhs, type);
    curBlock_->add(ins);
    return ins;
  }

  MDefinition* add(MDefinition* lhs, MDefinition* rhs, MIRType type) {
    if (inDeadCode()) {
      return nullptr;
    }
    auto* ins = MAdd::NewWasm(alloc(), lhs, rhs, type);
    curBlock_->add(ins);
    return ins;
  }

  bool mustPreserveNaN(MIRType type) { return IsFloatingPointType(type); }

  MDefinition* sub(MDefinition* lhs, MDefinition* rhs, MIRType type) {
    if (inDeadCode()) {
      return nullptr;
    }

    // wasm can't fold x - 0.0 because of NaN with custom payloads.
    MSub* ins = MSub::NewWasm(alloc(), lhs, rhs, type, mustPreserveNaN(type));
    curBlock_->add(ins);
    return ins;
  }

  MDefinition* nearbyInt(MDefinition* input, RoundingMode roundingMode) {
    if (inDeadCode()) {
      return nullptr;
    }

    auto* ins = MNearbyInt::New(alloc(), input, input->type(), roundingMode);
    curBlock_->add(ins);
    return ins;
  }

  MDefinition* minMax(MDefinition* lhs, MDefinition* rhs, MIRType type,
                      bool isMax) {
    if (inDeadCode()) {
      return nullptr;
    }

    if (mustPreserveNaN(type)) {
      // Convert signaling NaN to quiet NaNs.
      MDefinition* zero = constantZeroOfValType(ValType::fromMIRType(type));
      lhs = sub(lhs, zero, type);
      rhs = sub(rhs, zero, type);
    }

    MMinMax* ins = MMinMax::NewWasm(alloc(), lhs, rhs, type, isMax);
    curBlock_->add(ins);
    return ins;
  }

  MDefinition* mul(MDefinition* lhs, MDefinition* rhs, MIRType type,
                   MMul::Mode mode) {
    if (inDeadCode()) {
      return nullptr;
    }

    // wasm can't fold x * 1.0 because of NaN with custom payloads.
    auto* ins =
        MMul::NewWasm(alloc(), lhs, rhs, type, mode, mustPreserveNaN(type));
    curBlock_->add(ins);
    return ins;
  }

  MDefinition* div(MDefinition* lhs, MDefinition* rhs, MIRType type,
                   bool unsignd) {
    if (inDeadCode()) {
      return nullptr;
    }
    if (!unsignd && type == MIRType::Int32) {
      // Enforce the signedness of the operation by coercing the operands
      // to signed.  Otherwise, operands that "look" unsigned to Ion but
      // are not unsigned to Baldr (eg, unsigned right shifts) may lead to
      // the operation being executed unsigned.  Applies to mod() as well.
      //
      // Do this for Int32 only since Int64 is not subject to the same
      // issues.
      //
      // Note the offsets passed to MWasmBuiltinTruncateToInt32 are wrong here,
      // but it doesn't matter: they're not codegen'd to calls since inputs
      // already are int32.
      auto* lhs2 = createTruncateToInt32(lhs);
      curBlock_->add(lhs2);
      lhs = lhs2;
      auto* rhs2 = createTruncateToInt32(rhs);
      curBlock_->add(rhs2);
      rhs = rhs2;
    }

    // For x86 and arm we implement i64 div via c++ builtin.
    // A call to c++ builtin requires instance pointer.
#if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_ARM)
    if (type == MIRType::Int64) {
      auto* ins =
          MWasmBuiltinDivI64::New(alloc(), lhs, rhs, instancePointer_, unsignd,
                                  /*trapOnError=*/true, trapSiteDesc());
      curBlock_->add(ins);
      return ins;
    }
#endif

    auto* ins =
        MDiv::New(alloc(), lhs, rhs, type, unsignd, /*trapOnError=*/true,
                  trapSiteDesc(), mustPreserveNaN(type));
    curBlock_->add(ins);
    return ins;
  }

  MInstruction* createTruncateToInt32(MDefinition* op) {
    if (op->type() == MIRType::Double || op->type() == MIRType::Float32) {
      return MWasmBuiltinTruncateToInt32::New(alloc(), op, instancePointer_);
    }

    return MTruncateToInt32::New(alloc(), op);
  }

  MDefinition* mod(MDefinition* lhs, MDefinition* rhs, MIRType type,
                   bool unsignd) {
    if (inDeadCode()) {
      return nullptr;
    }
    if (!unsignd && type == MIRType::Int32) {
      // See block comment in div().
      auto* lhs2 = createTruncateToInt32(lhs);
      curBlock_->add(lhs2);
      lhs = lhs2;
      auto* rhs2 = createTruncateToInt32(rhs);
      curBlock_->add(rhs2);
      rhs = rhs2;
    }

    // For x86 and arm we implement i64 mod via c++ builtin.
    // A call to c++ builtin requires instance pointer.
#if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_ARM)
    if (type == MIRType::Int64) {
      auto* ins =
          MWasmBuiltinModI64::New(alloc(), lhs, rhs, instancePointer_, unsignd,
                                  /*trapOnError=*/true, trapSiteDesc());
      curBlock_->add(ins);
      return ins;
    }
#endif

    // Should be handled separately because we call BuiltinThunk for this case
    // and so, need to add the dependency from instancePointer.
    if (type == MIRType::Double) {
      auto* ins = MWasmBuiltinModD::New(alloc(), lhs, rhs, instancePointer_,
                                        type, bytecodeOffset());
      curBlock_->add(ins);
      return ins;
    }

    auto* ins = MMod::New(alloc(), lhs, rhs, type, unsignd,
                          /*trapOnError=*/true, trapSiteDesc());
    curBlock_->add(ins);
    return ins;
  }

  MDefinition* bitnot(MDefinition* op, MIRType type) {
    if (inDeadCode()) {
      return nullptr;
    }
    auto* ins = MBitNot::New(alloc(), op, type);
    curBlock_->add(ins);
    return ins;
  }

  MDefinition* select(MDefinition* trueExpr, MDefinition* falseExpr,
                      MDefinition* condExpr) {
    if (inDeadCode()) {
      return nullptr;
    }
    auto* ins = MWasmSelect::New(alloc(), trueExpr, falseExpr, condExpr);
    curBlock_->add(ins);
    return ins;
  }

  MDefinition* extendI32(MDefinition* op, bool isUnsigned) {
    if (inDeadCode()) {
      return nullptr;
    }
    auto* ins = MExtendInt32ToInt64::New(alloc(), op, isUnsigned);
    curBlock_->add(ins);
    return ins;
  }

  MDefinition* signExtend(MDefinition* op, uint32_t srcSize,
                          uint32_t targetSize) {
    if (inDeadCode()) {
      return nullptr;
    }
    MInstruction* ins;
    switch (targetSize) {
      case 4: {
        MSignExtendInt32::Mode mode;
        switch (srcSize) {
          case 1:
            mode = MSignExtendInt32::Byte;
            break;
          case 2:
            mode = MSignExtendInt32::Half;
            break;
          default:
            MOZ_CRASH("Bad sign extension");
        }
        ins = MSignExtendInt32::New(alloc(), op, mode);
        break;
      }
      case 8: {
        MSignExtendInt64::Mode mode;
        switch (srcSize) {
          case 1:
            mode = MSignExtendInt64::Byte;
            break;
          case 2:
            mode = MSignExtendInt64::Half;
            break;
          case 4:
            mode = MSignExtendInt64::Word;
            break;
          default:
            MOZ_CRASH("Bad sign extension");
        }
        ins = MSignExtendInt64::New(alloc(), op, mode);
        break;
      }
      default: {
        MOZ_CRASH("Bad sign extension");
      }
    }
    curBlock_->add(ins);
    return ins;
  }

  MDefinition* wrapI32(MDefinition* op) {
    if (inDeadCode()) {
      return nullptr;
    }
    auto* ins = MWrapInt64ToInt32::New(alloc(), op, /*bottomHalf=*/true);
    curBlock_->add(ins);
    return ins;
  }

  MDefinition* convertI64ToFloatingPoint(MDefinition* op, MIRType type,
                                         bool isUnsigned) {
    if (inDeadCode()) {
      return nullptr;
    }
#if defined(JS_CODEGEN_ARM)
    auto* ins = MBuiltinInt64ToFloatingPoint::New(
        alloc(), op, instancePointer_, type, bytecodeOffset(), isUnsigned);
#else
    auto* ins = MInt64ToFloatingPoint::New(alloc(), op, type, bytecodeOffset(),
                                           isUnsigned);
#endif
    curBlock_->add(ins);
    return ins;
  }

  MDefinition* rotate(MDefinition* input, MDefinition* count, MIRType type,
                      bool left) {
    if (inDeadCode()) {
      return nullptr;
    }
    auto* ins = MRotate::New(alloc(), input, count, type, left);
    curBlock_->add(ins);
    return ins;
  }

  template <class T>
  MDefinition* truncate(MDefinition* op, TruncFlags flags) {
    if (inDeadCode()) {
      return nullptr;
    }
    auto* ins = T::New(alloc(), op, flags, trapSiteDesc());
    curBlock_->add(ins);
    return ins;
  }

#if defined(JS_CODEGEN_ARM)
  MDefinition* truncateWithInstance(MDefinition* op, TruncFlags flags) {
    if (inDeadCode()) {
      return nullptr;
    }
    auto* ins = MWasmBuiltinTruncateToInt64::New(alloc(), op, instancePointer_,
                                                 flags, trapSiteDesc());
    curBlock_->add(ins);
    return ins;
  }
#endif

  MDefinition* compare(MDefinition* lhs, MDefinition* rhs, JSOp op,
                       MCompare::CompareType type) {
    if (inDeadCode()) {
      return nullptr;
    }
    auto* ins = MCompare::NewWasm(alloc(), lhs, rhs, op, type);
    curBlock_->add(ins);
    return ins;
  }

  void assign(unsigned slot, MDefinition* def) {
    if (inDeadCode()) {
      return;
    }
    curBlock_->setSlot(info().localSlot(slot), def);
  }

  MDefinition* compareIsNull(MDefinition* ref, JSOp compareOp) {
    MDefinition* nullVal = constantNullRef(MaybeRefType());
    if (!nullVal) {
      return nullptr;
    }
    return compare(ref, nullVal, compareOp, MCompare::Compare_WasmAnyRef);
  }

  [[nodiscard]] MDefinition* refAsNonNull(MDefinition* ref) {
    MOZ_ASSERT(!inDeadCode());
    auto* ins = MWasmRefAsNonNull::New(alloc(), ref, trapSiteDesc());
    if (!ins) {
      return nullptr;
    }
    curBlock_->add(ins);
    return ins;
  }

  [[nodiscard]] bool brOnNull(uint32_t relativeDepth, const DefVector& values,
                              const ResultType& type, MDefinition* condition,
                              BranchHint branchHint) {
    if (inDeadCode()) {
      return true;
    }

    MBasicBlock* fallthroughBlock = nullptr;
    if (!newBlock(curBlock_, &fallthroughBlock)) {
      return false;
    }

    MDefinition* check = compareIsNull(condition, JSOp::Eq);
    if (!check) {
      return false;
    }
    MTest* test = MTest::New(alloc(), check, nullptr, fallthroughBlock);
    if (!test || !addControlFlowPatch(test, relativeDepth,
                                      MTest::TrueBranchIndex, branchHint)) {
      return false;
    }

    if (!pushDefs(values)) {
      return false;
    }

    curBlock_->end(test);
    curBlock_ = fallthroughBlock;
    return true;
  }

  [[nodiscard]] bool brOnNonNull(uint32_t relativeDepth,
                                 const DefVector& values,
                                 const ResultType& type, MDefinition* condition,
                                 BranchHint branchHint) {
    if (inDeadCode()) {
      return true;
    }

    MBasicBlock* fallthroughBlock = nullptr;
    if (!newBlock(curBlock_, &fallthroughBlock)) {
      return false;
    }

    MDefinition* check = compareIsNull(condition, JSOp::Ne);
    if (!check) {
      return false;
    }
    MTest* test = MTest::New(alloc(), check, nullptr, fallthroughBlock);
    if (!test || !addControlFlowPatch(test, relativeDepth,
                                      MTest::TrueBranchIndex, branchHint)) {
      return false;
    }

    if (!pushDefs(values)) {
      return false;
    }

    curBlock_->end(test);
    curBlock_ = fallthroughBlock;
    return true;
  }

  MDefinition* refI31(MDefinition* input) {
    auto* ins = MWasmNewI31Ref::New(alloc(), input);
    curBlock_->add(ins);
    return ins;
  }

  MDefinition* i31Get(MDefinition* input, FieldWideningOp wideningOp) {
    auto* ins = MWasmI31RefGet::New(alloc(), input, wideningOp);
    curBlock_->add(ins);
    return ins;
  }

#ifdef ENABLE_WASM_SIMD
  // About Wasm SIMD as supported by Ion:
  //
  // The expectation is that Ion will only ever support SIMD on x86 and x64,
  // since ARMv7 will cease to be a tier-1 platform soon, and MIPS64 will never
  // implement SIMD.
  //
  // The division of the operations into MIR nodes reflects that expectation,
  // and is a good fit for x86/x64.  Should the expectation change we'll
  // possibly want to re-architect the SIMD support to be a little more general.
  //
  // Most SIMD operations map directly to a single MIR node that ultimately ends
  // up being expanded in the macroassembler.
  //
  // Some SIMD operations that do have a complete macroassembler expansion are
  // open-coded into multiple MIR nodes here; in some cases that's just
  // convenience, in other cases it may also allow them to benefit from Ion
  // optimizations.  The reason for the expansions will be documented by a
  // comment.

  // (v128,v128) -> v128 effect-free binary operations
  MDefinition* binarySimd128(MDefinition* lhs, MDefinition* rhs,
                             bool commutative, SimdOp op) {
    if (inDeadCode()) {
      return nullptr;
    }

    MOZ_ASSERT(lhs->type() == MIRType::Simd128 &&
               rhs->type() == MIRType::Simd128);

    auto* ins = MWasmBinarySimd128::New(alloc(), lhs, rhs, commutative, op);
    curBlock_->add(ins);
    return ins;
  }

  // (v128,i32) -> v128 effect-free shift operations
  MDefinition* shiftSimd128(MDefinition* lhs, MDefinition* rhs, SimdOp op) {
    if (inDeadCode()) {
      return nullptr;
    }

    MOZ_ASSERT(lhs->type() == MIRType::Simd128 &&
               rhs->type() == MIRType::Int32);

    int32_t maskBits;
    if (MacroAssembler::MustMaskShiftCountSimd128(op, &maskBits)) {
      MDefinition* mask = constantI32(maskBits);
      auto* rhs2 = MBitAnd::New(alloc(), rhs, mask, MIRType::Int32);
      curBlock_->add(rhs2);
      rhs = rhs2;
    }

    auto* ins = MWasmShiftSimd128::New(alloc(), lhs, rhs, op);
    curBlock_->add(ins);
    return ins;
  }

  // (v128,scalar,imm) -> v128
  MDefinition* replaceLaneSimd128(MDefinition* lhs, MDefinition* rhs,
                                  uint32_t laneIndex, SimdOp op) {
    if (inDeadCode()) {
      return nullptr;
    }

    MOZ_ASSERT(lhs->type() == MIRType::Simd128);

    auto* ins = MWasmReplaceLaneSimd128::New(alloc(), lhs, rhs, laneIndex, op);
    curBlock_->add(ins);
    return ins;
  }

  // (scalar) -> v128 effect-free unary operations
  MDefinition* scalarToSimd128(MDefinition* src, SimdOp op) {
    if (inDeadCode()) {
      return nullptr;
    }

    auto* ins = MWasmScalarToSimd128::New(alloc(), src, op);
    curBlock_->add(ins);
    return ins;
  }

  // (v128) -> v128 effect-free unary operations
  MDefinition* unarySimd128(MDefinition* src, SimdOp op) {
    if (inDeadCode()) {
      return nullptr;
    }

    MOZ_ASSERT(src->type() == MIRType::Simd128);
    auto* ins = MWasmUnarySimd128::New(alloc(), src, op);
    curBlock_->add(ins);
    return ins;
  }

  // (v128, imm) -> scalar effect-free unary operations
  MDefinition* reduceSimd128(MDefinition* src, SimdOp op, ValType outType,
                             uint32_t imm = 0) {
    if (inDeadCode()) {
      return nullptr;
    }

    MOZ_ASSERT(src->type() == MIRType::Simd128);
    auto* ins =
        MWasmReduceSimd128::New(alloc(), src, op, outType.toMIRType(), imm);
    curBlock_->add(ins);
    return ins;
  }

  // (v128, v128, v128) -> v128 effect-free operations
  MDefinition* ternarySimd128(MDefinition* v0, MDefinition* v1, MDefinition* v2,
                              SimdOp op) {
    if (inDeadCode()) {
      return nullptr;
    }

    MOZ_ASSERT(v0->type() == MIRType::Simd128 &&
               v1->type() == MIRType::Simd128 &&
               v2->type() == MIRType::Simd128);

    auto* ins = MWasmTernarySimd128::New(alloc(), v0, v1, v2, op);
    curBlock_->add(ins);
    return ins;
  }

  // (v128, v128, imm_v128) -> v128 effect-free operations
  MDefinition* shuffleSimd128(MDefinition* v1, MDefinition* v2, V128 control) {
    if (inDeadCode()) {
      return nullptr;
    }

    MOZ_ASSERT(v1->type() == MIRType::Simd128);
    MOZ_ASSERT(v2->type() == MIRType::Simd128);
    auto* ins = BuildWasmShuffleSimd128(
        alloc(), reinterpret_cast<int8_t*>(control.bytes), v1, v2);
    curBlock_->add(ins);
    return ins;
  }

  // Also see below for SIMD memory references

#endif  // ENABLE_WASM_SIMD

  /************************************************ Linear memory accesses */

  // For detailed information about memory accesses, see "Linear memory
  // addresses and bounds checking" in WasmMemory.cpp.

 private:
  // If the platform does not have a HeapReg, load the memory base from
  // instance.
  MDefinition* maybeLoadMemoryBase(uint32_t memoryIndex) {
#ifdef WASM_HAS_HEAPREG
    if (memoryIndex == 0) {
      return nullptr;
    }
#endif
    return memoryBase(memoryIndex);
  }

 public:
  // A value holding the memory base, whether that's HeapReg or some other
  // register.
  MDefinition* memoryBase(uint32_t memoryIndex) {
    AliasSet aliases = !codeMeta().memories[memoryIndex].canMovingGrow()
                           ? AliasSet::None()
                           : AliasSet::Load(AliasSet::WasmHeapMeta);
#ifdef WASM_HAS_HEAPREG
    if (memoryIndex == 0) {
      MWasmHeapReg* base = MWasmHeapReg::New(alloc(), aliases);
      curBlock_->add(base);
      return base;
    }
#endif
    uint32_t offset =
        memoryIndex == 0
            ? Instance::offsetOfMemory0Base()
            : (Instance::offsetInData(
                  codeMeta().offsetOfMemoryInstanceData(memoryIndex) +
                  offsetof(MemoryInstanceData, base)));
    MWasmLoadInstance* base = MWasmLoadInstance::New(
        alloc(), instancePointer_, offset, MIRType::Pointer, aliases);
    curBlock_->add(base);
    return base;
  }

 private:
  // If the bounds checking strategy requires it, load the bounds check limit
  // from the instance.
  MWasmLoadInstance* maybeLoadBoundsCheckLimit(uint32_t memoryIndex,
                                               MIRType type) {
    MOZ_ASSERT(type == MIRType::Int32 || type == MIRType::Int64);
    if (codeMeta().hugeMemoryEnabled(memoryIndex)) {
      return nullptr;
    }
    uint32_t offset =
        memoryIndex == 0
            ? Instance::offsetOfMemory0BoundsCheckLimit()
            : (Instance::offsetInData(
                  codeMeta().offsetOfMemoryInstanceData(memoryIndex) +
                  offsetof(MemoryInstanceData, boundsCheckLimit)));
    AliasSet aliases = !codeMeta().memories[memoryIndex].canMovingGrow()
                           ? AliasSet::None()
                           : AliasSet::Load(AliasSet::WasmHeapMeta);
    auto* load = MWasmLoadInstance::New(alloc(), instancePointer_, offset, type,
                                        aliases);
    curBlock_->add(load);
    return load;
  }

  // Return true if the access requires an alignment check.  If so, sets
  // *mustAdd to true if the offset must be added to the pointer before
  // checking.
  bool needAlignmentCheck(MemoryAccessDesc* access, MDefinition* base,
                          bool* mustAdd) {
    MOZ_ASSERT(!*mustAdd);

    if (!access->isAtomic()) {
      return false;
    }

    // If the EA is known and aligned it will need no checks.
    if (base->isConstant()) {
      // We only care about the low bits, so overflow is OK, as is chopping off
      // the high bits of an i64 pointer.
      uint32_t ptr = 0;
      if (isMem64(access->memoryIndex())) {
        ptr = uint32_t(base->toConstant()->toInt64());
      } else {
        ptr = base->toConstant()->toInt32();
      }
      if (((ptr + access->offset64()) & (access->byteSize() - 1)) == 0) {
        return false;
      }
    }

    // If the offset is aligned then the EA is just the pointer, for
    // the purposes of this check.
    *mustAdd = (access->offset64() & (access->byteSize() - 1)) != 0;
    return true;
  }

  // Fold a constant base into the offset and make the base 0, provided the
  // offset stays below the guard limit.  The reason for folding the base into
  // the offset rather than vice versa is that a small offset can be ignored
  // by both explicit bounds checking and bounds check elimination.
  void foldConstantPointer(MemoryAccessDesc* access, MDefinition** base) {
    PageSize pageSize = codeMeta().memories[access->memoryIndex()].pageSize();
    if (pageSize != PageSize::Standard) {
      return;
    }

    uint64_t offsetGuardLimit = GetMaxOffsetGuardLimit(
        codeMeta().hugeMemoryEnabled(access->memoryIndex()), pageSize);

    if ((*base)->isConstant()) {
      uint64_t basePtr = 0;
      if (isMem64(access->memoryIndex())) {
        basePtr = uint64_t((*base)->toConstant()->toInt64());
      } else {
        basePtr = uint64_t(int64_t((*base)->toConstant()->toInt32()));
      }

      uint64_t offset = access->offset64();
      if (offset < offsetGuardLimit && basePtr < offsetGuardLimit - offset) {
        offset += basePtr;
        access->setOffset32(uint32_t(offset));
        *base = isMem64(access->memoryIndex()) ? constantI64(int64_t(0))
                                               : constantI32(0);
      }
    }
  }

  // If the offset must be added because it is large or because the true EA must
  // be checked, compute the effective address, trapping on overflow.
  void maybeComputeEffectiveAddress(MemoryAccessDesc* access,
                                    MDefinition** base, bool mustAddOffset) {
    uint64_t offsetGuardLimit = GetMaxOffsetGuardLimit(
        codeMeta().hugeMemoryEnabled(access->memoryIndex()),
        codeMeta().memories[access->memoryIndex()].pageSize());

    if (access->offset64() >= offsetGuardLimit ||
        access->offset64() > UINT32_MAX || mustAddOffset ||
        !JitOptions.wasmFoldOffsets) {
      *base = computeEffectiveAddress(*base, access);
    }
  }

  MWasmLoadInstance* needBoundsCheck(uint32_t memoryIndex) {
    MOZ_RELEASE_ASSERT(codeMeta().memories[memoryIndex].pageSize() ==
                       PageSize::Standard);
#ifdef JS_64BIT
    // For 32-bit base pointers:
    //
    // If the bounds check uses the full 64 bits of the bounds check limit, then
    // the base pointer must be zero-extended to 64 bits before checking and
    // wrapped back to 32-bits after Spectre masking.  (And it's important that
    // the value we end up with has flowed through the Spectre mask.)
    //
    // If the memory's max size is known to be smaller than 64K pages exactly,
    // we can use a 32-bit check and avoid extension and wrapping.
    bool mem32LimitIs64Bits =
        isMem32(memoryIndex) &&
        !codeMeta().memories[memoryIndex].boundsCheckLimitIsAlways32Bits() &&
        MaxMemoryBytes(codeMeta().memories[memoryIndex].addressType(),
                       codeMeta().memories[memoryIndex].pageSize()) >=
            0x100000000;
#else
    // On 32-bit platforms we have no more than 2GB memory and the limit for a
    // 32-bit base pointer is never a 64-bit value.
    bool mem32LimitIs64Bits = false;
#endif
    return maybeLoadBoundsCheckLimit(memoryIndex,
                                     mem32LimitIs64Bits || isMem64(memoryIndex)
                                         ? MIRType::Int64
                                         : MIRType::Int32);
  }

  void performBoundsCheck(uint32_t memoryIndex, MDefinition** base,
                          MWasmLoadInstance* boundsCheckLimit) {
    // At the outset, actualBase could be the result of pretty much any integer
    // operation, or it could be the load of an integer constant.  If its type
    // is i32, we may assume the value has a canonical representation for the
    // platform, see doc block in MacroAssembler.h.
    MDefinition* actualBase = *base;

    // Extend an i32 index value to perform a 64-bit bounds check if the memory
    // can be 4GB or larger.
    bool extendAndWrapIndex =
        isMem32(memoryIndex) && boundsCheckLimit->type() == MIRType::Int64;
    if (extendAndWrapIndex) {
      auto* extended = MWasmExtendU32Index::New(alloc(), actualBase);
      curBlock_->add(extended);
      actualBase = extended;
    }

    auto* ins = MWasmBoundsCheck::New(alloc(), actualBase, boundsCheckLimit,
                                      trapSiteDesc(), MWasmBoundsCheck::Memory,
                                      memoryIndex);
    curBlock_->add(ins);
    actualBase = ins;

    // If we're masking, then we update *base to create a dependency chain
    // through the masked index.  But we will first need to wrap the index
    // value if it was extended above.
    if (JitOptions.spectreIndexMasking) {
      if (extendAndWrapIndex) {
        auto* wrapped = MWasmWrapU32Index::New(alloc(), actualBase);
        curBlock_->add(wrapped);
        actualBase = wrapped;
      }
      *base = actualBase;
    }
  }

  // Perform all necessary checking before a wasm heap access, based on the
  // attributes of the access and base pointer.
  //
  // For 64-bit indices on platforms that are limited to indices that fit into
  // 32 bits (all 32-bit platforms and mips64), this returns a bounds-checked
  // `base` that has type Int32.  Lowering code depends on this and will assert
  // that the base has this type.  See the end of this function.

  void checkOffsetAndAlignmentAndBounds(MemoryAccessDesc* access,
                                        MDefinition** base) {
    MOZ_ASSERT(!inDeadCode());

    // Attempt to fold a constant base pointer into the offset so as to simplify
    // the addressing expression. This may update *base.
    foldConstantPointer(access, base);

    // Determine whether an alignment check is needed and whether the offset
    // must be checked too.
    bool mustAddOffsetForAlignmentCheck = false;
    bool alignmentCheck =
        needAlignmentCheck(access, *base, &mustAddOffsetForAlignmentCheck);

    // If bounds checking or alignment checking requires it, compute the
    // effective address: add the offset into the pointer and trap on overflow.
    // This may update *base.
    maybeComputeEffectiveAddress(access, base, mustAddOffsetForAlignmentCheck);

    // Emit the alignment check if necessary; it traps if it fails.
    if (alignmentCheck) {
      curBlock_->add(MWasmAlignmentCheck::New(
          alloc(), *base, access->byteSize(), trapSiteDesc()));
    }

    // Emit the bounds check if necessary; it traps if it fails.  This may
    // update *base.
    MWasmLoadInstance* boundsCheckLimit =
        needBoundsCheck(access->memoryIndex());
    if (boundsCheckLimit) {
      performBoundsCheck(access->memoryIndex(), base, boundsCheckLimit);
    }

#ifndef JS_64BIT
    if (isMem64(access->memoryIndex())) {
      // We must have had an explicit bounds check (or one was elided if it was
      // proved redundant), and on 32-bit systems the index will for sure fit in
      // 32 bits: the max memory is 2GB.  So chop the index down to 32-bit to
      // simplify the back-end.
      MOZ_ASSERT((*base)->type() == MIRType::Int64);
      MOZ_ASSERT(!codeMeta().hugeMemoryEnabled(access->memoryIndex()));
      auto* chopped = MWasmWrapU32Index::New(alloc(), *base);
      MOZ_ASSERT(chopped->type() == MIRType::Int32);
      curBlock_->add(chopped);
      *base = chopped;
    }
#endif
  }

  bool isSmallerAccessForI64(ValType result, const MemoryAccessDesc* access) {
    if (result == ValType::I64 && access->byteSize() <= 4) {
      // These smaller accesses should all be zero-extending.
      MOZ_ASSERT(!isSignedIntType(access->type()));
      return true;
    }
    return false;
  }

 public:
  bool isMem32(uint32_t memoryIndex) {
    return codeMeta().memories[memoryIndex].addressType() == AddressType::I32;
  }
  bool isMem64(uint32_t memoryIndex) {
    return codeMeta().memories[memoryIndex].addressType() == AddressType::I64;
  }
  bool hugeMemoryEnabled(uint32_t memoryIndex) {
    return codeMeta().hugeMemoryEnabled(memoryIndex);
  }

  // Add the offset into the pointer to yield the EA; trap on overflow. Clears
  // the offset on the memory access as a result.
  MDefinition* computeEffectiveAddress(MDefinition* base,
                                       MemoryAccessDesc* access) {
    if (inDeadCode()) {
      return nullptr;
    }
    uint64_t offset = access->offset64();
    if (offset == 0) {
      return base;
    }
    auto* ins = MWasmAddOffset::New(alloc(), base, offset, trapSiteDesc());
    curBlock_->add(ins);
    access->clearOffset();
    return ins;
  }

  MDefinition* load(MDefinition* base, MemoryAccessDesc* access,
                    ValType result) {
    if (inDeadCode()) {
      return nullptr;
    }

    MDefinition* memoryBase = maybeLoadMemoryBase(access->memoryIndex());
    checkOffsetAndAlignmentAndBounds(access, &base);
#ifndef JS_64BIT
    MOZ_ASSERT(base->type() == MIRType::Int32);
#endif
    MInstruction* load =
        MWasmLoad::New(alloc(), memoryBase, base, *access, result.toMIRType());
    if (!load) {
      return nullptr;
    }
    curBlock_->add(load);
    return load;
  }

  [[nodiscard]] bool store(MDefinition* base, MemoryAccessDesc* access,
                           MDefinition* v) {
    if (inDeadCode()) {
      return true;
    }

    MDefinition* memoryBase = maybeLoadMemoryBase(access->memoryIndex());
    checkOffsetAndAlignmentAndBounds(access, &base);
#ifndef JS_64BIT
    MOZ_ASSERT(base->type() == MIRType::Int32);
#endif
    MInstruction* store =
        MWasmStore::New(alloc(), memoryBase, base, *access, v);
    if (!store) {
      return false;
    }
    curBlock_->add(store);
    return true;
  }

  MDefinition* atomicCompareExchangeHeap(MDefinition* base,
                                         MemoryAccessDesc* access,
                                         ValType result, MDefinition* oldv,
                                         MDefinition* newv) {
    if (inDeadCode()) {
      return nullptr;
    }

    checkOffsetAndAlignmentAndBounds(access, &base);
#ifndef JS_64BIT
    MOZ_ASSERT(base->type() == MIRType::Int32);
#endif

    if (isSmallerAccessForI64(result, access)) {
      auto* cvtOldv =
          MWrapInt64ToInt32::New(alloc(), oldv, /*bottomHalf=*/true);
      curBlock_->add(cvtOldv);
      oldv = cvtOldv;

      auto* cvtNewv =
          MWrapInt64ToInt32::New(alloc(), newv, /*bottomHalf=*/true);
      curBlock_->add(cvtNewv);
      newv = cvtNewv;
    }

    MDefinition* memoryBase = maybeLoadMemoryBase(access->memoryIndex());
    MInstruction* cas = MWasmCompareExchangeHeap::New(
        alloc(), bytecodeOffset(), memoryBase, base, *access, oldv, newv,
        instancePointer_);
    if (!cas) {
      return nullptr;
    }
    curBlock_->add(cas);

    if (isSmallerAccessForI64(result, access)) {
      cas = MExtendInt32ToInt64::New(alloc(), cas, true);
      curBlock_->add(cas);
    }

    return cas;
  }

  MDefinition* atomicExchangeHeap(MDefinition* base, MemoryAccessDesc* access,
                                  ValType result, MDefinition* value) {
    if (inDeadCode()) {
      return nullptr;
    }

    checkOffsetAndAlignmentAndBounds(access, &base);
#ifndef JS_64BIT
    MOZ_ASSERT(base->type() == MIRType::Int32);
#endif

    if (isSmallerAccessForI64(result, access)) {
      auto* cvtValue =
          MWrapInt64ToInt32::New(alloc(), value, /*bottomHalf=*/true);
      curBlock_->add(cvtValue);
      value = cvtValue;
    }

    MDefinition* memoryBase = maybeLoadMemoryBase(access->memoryIndex());
    MInstruction* xchg =
        MWasmAtomicExchangeHeap::New(alloc(), bytecodeOffset(), memoryBase,
                                     base, *access, value, instancePointer_);
    if (!xchg) {
      return nullptr;
    }
    curBlock_->add(xchg);

    if (isSmallerAccessForI64(result, access)) {
      xchg = MExtendInt32ToInt64::New(alloc(), xchg, true);
      curBlock_->add(xchg);
    }

    return xchg;
  }

  MDefinition* atomicBinopHeap(AtomicOp op, MDefinition* base,
                               MemoryAccessDesc* access, ValType result,
                               MDefinition* value) {
    if (inDeadCode()) {
      return nullptr;
    }

    checkOffsetAndAlignmentAndBounds(access, &base);
#ifndef JS_64BIT
    MOZ_ASSERT(base->type() == MIRType::Int32);
#endif

    if (isSmallerAccessForI64(result, access)) {
      auto* cvtValue =
          MWrapInt64ToInt32::New(alloc(), value, /*bottomHalf=*/true);
      curBlock_->add(cvtValue);
      value = cvtValue;
    }

    MDefinition* memoryBase = maybeLoadMemoryBase(access->memoryIndex());
    MInstruction* binop =
        MWasmAtomicBinopHeap::New(alloc(), bytecodeOffset(), op, memoryBase,
                                  base, *access, value, instancePointer_);
    if (!binop) {
      return nullptr;
    }
    curBlock_->add(binop);

    if (isSmallerAccessForI64(result, access)) {
      binop = MExtendInt32ToInt64::New(alloc(), binop, true);
      curBlock_->add(binop);
    }

    return binop;
  }

#ifdef ENABLE_WASM_SIMD
  MDefinition* loadSplatSimd128(Scalar::Type viewType,
                                const LinearMemoryAddress<MDefinition*>& addr,
                                wasm::SimdOp splatOp) {
    if (inDeadCode()) {
      return nullptr;
    }

    MemoryAccessDesc access(addr.memoryIndex, viewType, addr.align, addr.offset,
                            trapSiteDesc(),
                            hugeMemoryEnabled(addr.memoryIndex));

    // Generate better code (on x86)
    // If AVX2 is enabled, more broadcast operators are available.
    if (viewType == Scalar::Float64
#  if defined(JS_CODEGEN_X64) || defined(JS_CODEGEN_X86)
        || (js::jit::CPUInfo::IsAVX2Present() &&
            (viewType == Scalar::Uint8 || viewType == Scalar::Uint16 ||
             viewType == Scalar::Float32))
#  endif
    ) {
      access.setSplatSimd128Load();
      return load(addr.base, &access, ValType::V128);
    }

    ValType resultType = ValType::I32;
    if (viewType == Scalar::Float32) {
      resultType = ValType::F32;
      splatOp = wasm::SimdOp::F32x4Splat;
    }
    auto* scalar = load(addr.base, &access, resultType);
    if (!inDeadCode() && !scalar) {
      return nullptr;
    }
    return scalarToSimd128(scalar, splatOp);
  }

  MDefinition* loadExtendSimd128(const LinearMemoryAddress<MDefinition*>& addr,
                                 wasm::SimdOp op) {
    if (inDeadCode()) {
      return nullptr;
    }

    // Generate better code (on x86) by loading as a double with an
    // operation that sign extends directly.
    MemoryAccessDesc access(addr.memoryIndex, Scalar::Float64, addr.align,
                            addr.offset, trapSiteDesc(),
                            hugeMemoryEnabled(addr.memoryIndex));
    access.setWidenSimd128Load(op);
    return load(addr.base, &access, ValType::V128);
  }

  MDefinition* loadZeroSimd128(Scalar::Type viewType, size_t numBytes,
                               const LinearMemoryAddress<MDefinition*>& addr) {
    if (inDeadCode()) {
      return nullptr;
    }

    MemoryAccessDesc access(addr.memoryIndex, viewType, addr.align, addr.offset,
                            trapSiteDesc(),
                            hugeMemoryEnabled(addr.memoryIndex));
    access.setZeroExtendSimd128Load();
    return load(addr.base, &access, ValType::V128);
  }

  MDefinition* loadLaneSimd128(uint32_t laneSize,
                               const LinearMemoryAddress<MDefinition*>& addr,
                               uint32_t laneIndex, MDefinition* src) {
    if (inDeadCode()) {
      return nullptr;
    }

    MemoryAccessDesc access(addr.memoryIndex, Scalar::Simd128, addr.align,
                            addr.offset, trapSiteDesc(),
                            hugeMemoryEnabled(addr.memoryIndex));
    MDefinition* memoryBase = maybeLoadMemoryBase(access.memoryIndex());
    MDefinition* base = addr.base;
    checkOffsetAndAlignmentAndBounds(&access, &base);
#  ifndef JS_64BIT
    MOZ_ASSERT(base->type() == MIRType::Int32);
#  endif
    MInstruction* load = MWasmLoadLaneSimd128::New(
        alloc(), memoryBase, base, access, laneSize, laneIndex, src);
    if (!load) {
      return nullptr;
    }
    curBlock_->add(load);
    return load;
  }

  [[nodiscard]] bool storeLaneSimd128(
      uint32_t laneSize, const LinearMemoryAddress<MDefinition*>& addr,
      uint32_t laneIndex, MDefinition* src) {
    if (inDeadCode()) {
      return true;
    }
    MemoryAccessDesc access(addr.memoryIndex, Scalar::Simd128, addr.align,
                            addr.offset, trapSiteDesc(),
                            hugeMemoryEnabled(addr.memoryIndex));
    MDefinition* memoryBase = maybeLoadMemoryBase(access.memoryIndex());
    MDefinition* base = addr.base;
    checkOffsetAndAlignmentAndBounds(&access, &base);
#  ifndef JS_64BIT
    MOZ_ASSERT(base->type() == MIRType::Int32);
#  endif
    MInstruction* store = MWasmStoreLaneSimd128::New(
        alloc(), memoryBase, base, access, laneSize, laneIndex, src);
    if (!store) {
      return false;
    }
    curBlock_->add(store);
    return true;
  }
#endif  // ENABLE_WASM_SIMD

  /************************************************ Global variable accesses */

  MDefinition* loadGlobalVar(const GlobalDesc& global) {
    if (inDeadCode()) {
      return nullptr;
    }

    MInstruction* load;
    if (global.isIndirect()) {
      // Pull a pointer to the value out of Instance::globalArea, then
      // load from that pointer.  Note that the pointer is immutable
      // even though the value it points at may change, hence the use of
      // |true| for the first node's |isConst| value, irrespective of
      // the |isConst| formal parameter to this method.  The latter
      // applies to the denoted value as a whole.
      auto* cellPtr = MWasmLoadInstanceDataField::New(
          alloc(), MIRType::Pointer, global.offset(),
          /*isConst=*/true, instancePointer_);
      curBlock_->add(cellPtr);
      load = MWasmLoadGlobalCell::New(alloc(), global.type().toMIRType(),
                                      cellPtr, global.type());
    } else {
      // Pull the value directly out of Instance::globalArea.
      load = MWasmLoadInstanceDataField::New(
          alloc(), global.type().toMIRType(), global.offset(),
          !global.isMutable(), instancePointer_,
          global.type().toMaybeRefType());
    }
    curBlock_->add(load);
    return load;
  }

  [[nodiscard]] bool storeGlobalVar(uint32_t bytecodeOffset,
                                    const GlobalDesc& global, MDefinition* v) {
    if (inDeadCode()) {
      return true;
    }

    if (global.isIndirect()) {
      // Pull a pointer to the value out of Instance::globalArea, then
      // store through that pointer.
      auto* valueAddr = MWasmLoadInstanceDataField::New(
          alloc(), MIRType::Pointer, global.offset(),
          /*isConstant=*/true, instancePointer_);
      curBlock_->add(valueAddr);

      // Handle a store to a ref-typed field specially
      if (global.type().toMIRType() == MIRType::WasmAnyRef) {
        // Load the previous value for the post-write barrier
        MOZ_ASSERT(v->type() == MIRType::WasmAnyRef);
        auto* prevValue = MWasmLoadGlobalCell::New(alloc(), MIRType::WasmAnyRef,
                                                   valueAddr, global.type());
        curBlock_->add(prevValue);

        // Store the new value
        auto* store =
            MWasmStoreRef::New(alloc(), instancePointer_, valueAddr,
                               /*valueOffset=*/0, v, AliasSet::WasmGlobalCell,
                               WasmPreBarrierKind::Normal);
        curBlock_->add(store);

        // Call the post-write barrier
        return postBarrierEdgePrecise(bytecodeOffset, valueAddr, prevValue);
      }

      auto* store = MWasmStoreGlobalCell::New(alloc(), v, valueAddr);
      curBlock_->add(store);
      return true;
    }
    // Or else store the value directly in Instance::globalArea.

    // Handle a store to a ref-typed field specially
    if (global.type().toMIRType() == MIRType::WasmAnyRef) {
      // Compute the address of the ref-typed global
      auto* valueAddr = MWasmDerivedPointer::New(
          alloc(), instancePointer_,
          wasm::Instance::offsetInData(global.offset()));
      curBlock_->add(valueAddr);

      // Load the previous value for the post-write barrier
      MOZ_ASSERT(v->type() == MIRType::WasmAnyRef);
      auto* prevValue = MWasmLoadGlobalCell::New(alloc(), MIRType::WasmAnyRef,
                                                 valueAddr, global.type());
      curBlock_->add(prevValue);

      // Store the new value
      auto* store =
          MWasmStoreRef::New(alloc(), instancePointer_, valueAddr,
                             /*valueOffset=*/0, v, AliasSet::WasmInstanceData,
                             WasmPreBarrierKind::Normal);
      curBlock_->add(store);

      // Call the post-write barrier
      return postBarrierEdgePrecise(bytecodeOffset, valueAddr, prevValue);
    }

    auto* store = MWasmStoreInstanceDataField::New(alloc(), global.offset(), v,
                                                   instancePointer_);
    curBlock_->add(store);
    return true;
  }

  // Load the slot on the instance where the result of `ref.func` is cached.
  // This may be null if a function reference for this function has not been
  // asked for yet.
  MDefinition* loadCachedRefFunc(uint32_t funcIndex) {
    uint32_t exportedFuncIndex = codeMeta().findFuncExportIndex(funcIndex);
    MWasmLoadInstanceDataField* refFunc = MWasmLoadInstanceDataField::New(
        alloc(), MIRType::WasmAnyRef,
        codeMeta().offsetOfFuncExportInstanceData(exportedFuncIndex) +
            offsetof(FuncExportInstanceData, func),
        true, instancePointer_);
    curBlock_->add(refFunc);
    return refFunc;
  }

  MDefinition* loadTableField(uint32_t tableIndex, unsigned fieldOffset,
                              MIRType type) {
    uint32_t instanceDataOffset = wasm::Instance::offsetInData(
        codeMeta().offsetOfTableInstanceData(tableIndex) + fieldOffset);
    auto* load =
        MWasmLoadInstance::New(alloc(), instancePointer_, instanceDataOffset,
                               type, AliasSet::Load(AliasSet::WasmTableMeta));
    curBlock_->add(load);
    return load;
  }

  MDefinition* loadTableLength(uint32_t tableIndex) {
    const TableDesc& table = codeMeta().tables[tableIndex];
    if (table.maximumLength().isSome() &&
        table.initialLength() == *table.maximumLength()) {
      return table.addressType() == AddressType::I64
                 ? constantI64(int64_t(table.initialLength()))
                 : constantI32(int32_t(table.initialLength()));
    }
    return loadTableField(tableIndex, offsetof(TableInstanceData, length),
                          table.addressType() == AddressType::I64
                              ? MIRType::Int64
                              : MIRType::Int32);
  }

  MDefinition* loadTableElements(uint32_t tableIndex) {
    return loadTableField(tableIndex, offsetof(TableInstanceData, elements),
                          MIRType::Pointer);
  }

  // Clamps a table address into i32 range. If the value is too large to fit in
  // an i32, it will be replaced with UINT32_MAX so that it will always fail a
  // 32-bit bounds check. Consider using an actual 64-bit bounds check if
  // possible.
  MDefinition* clampTableAddressToI32(AddressType addressType,
                                      MDefinition* address) {
    switch (addressType) {
      case AddressType::I32:
        return address;
      case AddressType::I64:
        auto* clamp = MWasmClampTable64Address::New(alloc(), address);
        if (!clamp) {
          return nullptr;
        }
        curBlock_->add(clamp);
        return clamp;
    }
    MOZ_CRASH("unknown address type");
  }

  MDefinition* boundsCheckAndWrapTableAddress(uint32_t tableIndex,
                                              MDefinition* address) {
    const TableDesc& table = codeMeta().tables[tableIndex];

    // Load the table length and perform a bounds check with spectre index
    // masking
    auto* length = loadTableLength(tableIndex);
    auto* check =
        MWasmBoundsCheck::New(alloc(), address, length, trapSiteDesc(),
                              MWasmBoundsCheck::Table, tableIndex);
    curBlock_->add(check);
    if (JitOptions.spectreIndexMasking) {
      address = check;
    }

    if (table.addressType() == AddressType::I64) {
      auto* address32 =
          MWrapInt64ToInt32::New(alloc(), address, /*bottomHalf=*/true);
      curBlock_->add(address32);
      return address32;
    }
    return address;
  };

  MDefinition* tableGetAnyRef(uint32_t tableIndex, MDefinition* address) {
    const TableDesc& table = codeMeta().tables[tableIndex];

    auto* address32 = boundsCheckAndWrapTableAddress(tableIndex, address);

    // Load the table elements and load the element
    auto* elements = loadTableElements(tableIndex);
    auto* element = MWasmLoadTableElement::New(alloc(), elements, address32,
                                               table.elemType());
    curBlock_->add(element);
    return element;
  }

  [[nodiscard]] bool tableSetAnyRef(uint32_t tableIndex, MDefinition* address,
                                    MDefinition* value,
                                    uint32_t bytecodeOffset) {
    const TableDesc& table = codeMeta().tables[tableIndex];

    auto* address32 = boundsCheckAndWrapTableAddress(tableIndex, address);

    // Load the table elements
    auto* elements = loadTableElements(tableIndex);

    // Load the previous value
    auto* prevValue = MWasmLoadTableElement::New(alloc(), elements, address32,
                                                 table.elemType());
    curBlock_->add(prevValue);

    // Compute the value's location for the post barrier
    auto* loc = MWasmDerivedIndexPointer::New(alloc(), elements, address32,
                                              ScalePointer);
    curBlock_->add(loc);

    // Store the new value
    auto* store = MWasmStoreRef::New(
        alloc(), instancePointer_, loc, /*valueOffset=*/0, value,
        AliasSet::WasmTableElement, WasmPreBarrierKind::Normal);
    curBlock_->add(store);

    // Perform the post barrier
    return postBarrierEdgePrecise(bytecodeOffset, loc, prevValue);
  }

  void addInterruptCheck() {
    if (inDeadCode()) {
      return;
    }
    curBlock_->add(
        MWasmInterruptCheck::New(alloc(), instancePointer_, trapSiteDesc()));
  }

  // Perform a post-write barrier to update the generational store buffer. This
  // version stores the entire containing object (e.g. a struct) rather than a
  // single edge.
  [[nodiscard]] bool postBarrierWholeCell(uint32_t bytecodeOffset,
                                          MDefinition* object,
                                          MDefinition* newValue) {
    auto* barrier = MWasmPostWriteBarrierWholeCell::New(
        alloc(), instancePointer_, object, newValue);
    if (!barrier) {
      return false;
    }
    curBlock_->add(barrier);
    return true;
  }

  // Perform a post-write barrier to update the generational store buffer. This
  // version tracks a single tenured -> nursery edge, and will remove a previous
  // store buffer entry if it is no longer needed.
  [[nodiscard]] bool postBarrierEdgePrecise(uint32_t bytecodeOffset,
                                            MDefinition* valueAddr,
                                            MDefinition* value) {
    return emitInstanceCall2(bytecodeOffset, SASigPostBarrierEdgePrecise,
                             valueAddr, value);
  }

  // Perform a post-write barrier to update the generational store buffer. This
  // version does not remove a previous store buffer entry if it is no longer
  // needed.
  [[nodiscard]] bool postBarrierEdgeAtIndex(uint32_t bytecodeOffset,
                                            MDefinition* object,
                                            MDefinition* valueBase,
                                            MDefinition* index, uint32_t scale,
                                            MDefinition* newValue) {
    auto* barrier = MWasmPostWriteBarrierEdgeAtIndex::New(
        alloc(), instancePointer_, object, valueBase, index, scale, newValue);
    if (!barrier) {
      return false;
    }
    curBlock_->add(barrier);
    return true;
  }

  /***************************************************************** Calls */

  // The IonMonkey backend maintains a single stack offset (from the stack
  // pointer to the base of the frame) by adding the total amount of spill
  // space required plus the maximum stack required for argument passing.
  // Since we do not use IonMonkey's MPrepareCall/MPassArg/MCall, we must
  // manually accumulate, for the entire function, the maximum required stack
  // space for argument passing. (This is passed to the CodeGenerator via
  // MIRGenerator::maxWasmStackArgBytes.) This is just be the maximum of the
  // stack space required for each individual call (as determined by the call
  // ABI).

  [[nodiscard]]
  bool passInstanceCallArg(MIRType instanceType, CallCompileState* callState) {
    if (inDeadCode()) {
      return true;
    }

    // Should only pass an instance once.  And it must be a non-GC pointer.
    MOZ_ASSERT(callState->instanceArg == ABIArg());
    MOZ_ASSERT(instanceType == MIRType::Pointer);
    callState->instanceArg = callState->abi.next(MIRType::Pointer);
    return true;
  }

  // Do not call this directly.  Call one of the passCallArg() variants instead.
  [[nodiscard]]
  bool passCallArgWorker(MDefinition* argDef, MIRType type,
                         CallCompileState* callState) {
    MOZ_ASSERT(argDef->type() == type);

    // Calling a softFP function requires moving our floats into GPRs.
    if (!callState->hardFP &&
        (type == MIRType::Double || type == MIRType::Float32)) {
      MIRType softType =
          (type == MIRType::Double) ? MIRType::Int64 : MIRType::Int32;
      auto* softDef = MReinterpretCast::New(alloc(), argDef, softType);
      if (!softDef) {
        return false;
      }
      curBlock_->add(softDef);
      argDef = softDef;
    }

    ABIArg arg = callState->abi.next(type);
    switch (arg.kind()) {
#ifdef JS_CODEGEN_REGISTER_PAIR
      case ABIArg::GPR_PAIR: {
        auto mirLow =
            MWrapInt64ToInt32::New(alloc(), argDef, /* bottomHalf = */ true);
        curBlock_->add(mirLow);
        auto mirHigh =
            MWrapInt64ToInt32::New(alloc(), argDef, /* bottomHalf = */ false);
        curBlock_->add(mirHigh);
        return callState->regArgs.append(
                   MWasmCallBase::Arg(AnyRegister(arg.gpr64().low), mirLow)) &&
               callState->regArgs.append(
                   MWasmCallBase::Arg(AnyRegister(arg.gpr64().high), mirHigh));
      }
#endif
      case ABIArg::GPR:
      case ABIArg::FPU:
        return callState->regArgs.append(MWasmCallBase::Arg(arg.reg(), argDef));
      case ABIArg::Stack: {
        auto* mir =
            MWasmStackArg::New(alloc(), arg.offsetFromArgBase(), argDef);
        curBlock_->add(mir);
        return true;
      }
      case ABIArg::Uninitialized:
        MOZ_ASSERT_UNREACHABLE("Uninitialized ABIArg kind");
    }
    MOZ_CRASH("Unknown ABIArg kind.");
  }

  template <typename VecT>
  [[nodiscard]]
  bool passCallArgs(const DefVector& argDefs, const VecT& types,
                    CallCompileState* callState) {
    MOZ_ASSERT(argDefs.length() == types.length());
    for (uint32_t i = 0; i < argDefs.length(); i++) {
      MDefinition* def = argDefs[i];
      ValType type = types[i];
      if (!passCallArg(def, type, callState)) {
        return false;
      }
    }
    return true;
  }

  [[nodiscard]]
  bool passCallArg(MDefinition* argDef, MIRType type,
                   CallCompileState* callState) {
    if (inDeadCode()) {
      return true;
    }
    return passCallArgWorker(argDef, type, callState);
  }

  [[nodiscard]]
  bool passCallArg(MDefinition* argDef, ValType type,
                   CallCompileState* callState) {
    if (inDeadCode()) {
      return true;
    }
    return passCallArgWorker(argDef, type.toMIRType(), callState);
  }

  // If the call returns results on the stack, prepare a stack area to receive
  // them, and pass the address of the stack area to the callee as an additional
  // argument.
  [[nodiscard]]
  bool passStackResultAreaCallArg(const ResultType& resultType,
                                  CallCompileState* callState) {
    if (inDeadCode()) {
      return true;
    }
    ABIResultIter iter(resultType);
    while (!iter.done() && iter.cur().inRegister()) {
      iter.next();
    }
    if (iter.done()) {
      // No stack results.
      return true;
    }

    // The builtin ABI only supports a single result value, so it doesn't
    // use stack results.
    MOZ_ASSERT(callState->abiKind == ABIKind::Wasm);

    auto* stackResultArea = MWasmStackResultArea::New(alloc());
    if (!stackResultArea) {
      return false;
    }
    if (!stackResultArea->init(alloc(), iter.remaining())) {
      return false;
    }
    for (uint32_t base = iter.index(); !iter.done(); iter.next()) {
      MWasmStackResultArea::StackResult loc(iter.cur().stackOffset(),
                                            iter.cur().type().toMIRType());
      stackResultArea->initResult(iter.index() - base, loc);
    }
    curBlock_->add(stackResultArea);
    MDefinition* def = callState->returnCall ? (MDefinition*)stackResultPointer_
                                             : (MDefinition*)stackResultArea;
    if (!passCallArg(def, MIRType::StackResults, callState)) {
      return false;
    }
    callState->stackResultArea = stackResultArea;
    return true;
  }

  [[nodiscard]]
  bool finishCallArgs(CallCompileState* callState) {
    if (inDeadCode()) {
      return true;
    }

    if (!callState->regArgs.append(
            MWasmCallBase::Arg(AnyRegister(InstanceReg), instancePointer_))) {
      return false;
    }

    uint32_t stackBytes = callState->abi.stackBytesConsumedSoFar();

    maxStackArgBytes_ = std::max(maxStackArgBytes_, stackBytes);
    return true;
  }

  [[nodiscard]]
  bool emitCallArgs(const FuncType& funcType, const DefVector& args,
                    CallCompileState* callState) {
    for (size_t i = 0, n = funcType.args().length(); i < n; ++i) {
      if (!mirGen().ensureBallast()) {
        return false;
      }
      if (!passCallArg(args[i], funcType.args()[i], callState)) {
        return false;
      }
    }

    ResultType resultType = ResultType::Vector(funcType.results());
    if (!passStackResultAreaCallArg(resultType, callState)) {
      return false;
    }

    return finishCallArgs(callState);
  }

  [[nodiscard]]
  bool collectBuiltinCallResult(MIRType type, MDefinition** result,
                                CallCompileState* callState) {
    MInstruction* def;
    switch (type) {
      case MIRType::Int32:
        def = MWasmRegisterResult::New(alloc(), MIRType::Int32, ReturnReg);
        break;
      case MIRType::Int64:
        def = MWasmRegister64Result::New(alloc(), ReturnReg64);
        break;
      case MIRType::Float32: {
        if (callState->abiKind == ABIKind::System) {
          def = MWasmSystemFloatRegisterResult::New(
              alloc(), type, ReturnFloat32Reg, callState->hardFP);
        } else {
          def = MWasmFloatRegisterResult::New(alloc(), MIRType::Float32,
                                              ReturnFloat32Reg);
        }
        break;
      }
      case MIRType::Double: {
        if (callState->abiKind == ABIKind::System) {
          def = MWasmSystemFloatRegisterResult::New(
              alloc(), type, ReturnDoubleReg, callState->hardFP);
        } else {
          def = MWasmFloatRegisterResult::New(alloc(), MIRType::Double,
                                              ReturnDoubleReg);
        }
        break;
      }
#ifdef ENABLE_WASM_SIMD
      case MIRType::Simd128:
        MOZ_CRASH("SIMD128 not supported in builtin ABI");
#endif
      case MIRType::WasmAnyRef:
        def = MWasmRegisterResult::New(alloc(), MIRType::WasmAnyRef, ReturnReg);
        break;
      case MIRType::None:
        MOZ_ASSERT(result == nullptr, "Not expecting any results created");
        return true;
      default:
        MOZ_CRASH("unexpected MIRType result for builtin call");
    }

    if (!def) {
      return false;
    }

    curBlock_->add(def);
    *result = def;

    return true;
  }

  [[nodiscard]]
  bool collectWasmCallResults(const ResultType& type,
                              CallCompileState* callState, DefVector* results) {
    // This function uses wasm::ABIResultIter which does not handle the system
    // ABI. Use collectBuiltinCallResult instead for builtin calls.
    MOZ_ASSERT(callState->abiKind == ABIKind::Wasm);
    MOZ_ASSERT(callState->hardFP);

    if (!results->reserve(type.length())) {
      return false;
    }

    // The result iterator goes in the order in which results would be popped
    // off; we want the order in which they would be pushed.
    ABIResultIter iter(type);
    uint32_t stackResultCount = 0;
    while (!iter.done()) {
      if (iter.cur().onStack()) {
        stackResultCount++;
      }
      iter.next();
    }

    for (iter.switchToPrev(); !iter.done(); iter.prev()) {
      if (!mirGen().ensureBallast()) {
        return false;
      }
      const ABIResult& result = iter.cur();
      MInstruction* def;
      if (result.inRegister()) {
        switch (result.type().kind()) {
          case wasm::ValType::I32:
            def =
                MWasmRegisterResult::New(alloc(), MIRType::Int32, result.gpr());
            break;
          case wasm::ValType::I64:
            def = MWasmRegister64Result::New(alloc(), result.gpr64());
            break;
          case wasm::ValType::F32:
            def = MWasmFloatRegisterResult::New(alloc(), MIRType::Float32,
                                                result.fpr());
            break;
          case wasm::ValType::F64:
            def = MWasmFloatRegisterResult::New(alloc(), MIRType::Double,
                                                result.fpr());
            break;
          case wasm::ValType::Ref:
            def = MWasmRegisterResult::New(alloc(), MIRType::WasmAnyRef,
                                           result.gpr(),
                                           result.type().toMaybeRefType());
            break;
          case wasm::ValType::V128:
#ifdef ENABLE_WASM_SIMD
            def = MWasmFloatRegisterResult::New(alloc(), MIRType::Simd128,
                                                result.fpr());
#else
            return this->fail("Ion has no SIMD support yet");
#endif
        }
      } else {
        MOZ_ASSERT(callState->stackResultArea);
        MOZ_ASSERT(stackResultCount);
        uint32_t idx = --stackResultCount;
        def = MWasmStackResult::New(alloc(), callState->stackResultArea, idx);
      }

      if (!def) {
        return false;
      }
      curBlock_->add(def);
      results->infallibleAppend(def);
    }

    MOZ_ASSERT(results->length() == type.length());

    return true;
  }

  [[nodiscard]]
  bool call(CallCompileState* callState, const CallSiteDesc& desc,
            const CalleeDesc& callee, const ArgTypeVector& argTypes,
            MDefinition* addressOrRef = nullptr) {
    if (!beginCatchableCall(callState)) {
      return false;
    }

    MInstruction* ins;
    if (callState->isCatchable()) {
      ins = MWasmCallCatchable::New(
          alloc(), desc, callee, callState->regArgs,
          StackArgAreaSizeUnaligned(argTypes, callState->abiKind),
          callState->tryNoteIndex, callState->fallthroughBlock,
          callState->prePadBlock, addressOrRef);
    } else {
      ins = MWasmCallUncatchable::New(
          alloc(), desc, callee, callState->regArgs,
          StackArgAreaSizeUnaligned(argTypes, callState->abiKind),
          addressOrRef);
    }
    if (!ins) {
      return false;
    }
    curBlock_->add(ins);

    return finishCatchableCall(callState);
  }

  [[nodiscard]]
  CallRefHint auditInlineableCallees(InliningHeuristics::CallKind kind,
                                     CallRefHint hints) {
    // Takes candidates for inlining as provided in `hints`, and returns a
    // subset (or all) of them for which inlining is approved.  To indicate
    // that they are all disallowed, return an empty CallRefHint.

    MOZ_ASSERT_IF(kind == InliningHeuristics::CallKind::Direct,
                  hints.length() == 1);

    // We only support inlining when lazy tiering. This is currently a
    // requirement because we need the full module bytecode and function
    // definition ranges, which are not available in other modes.
    if (compilerEnv().mode() != CompileMode::LazyTiering) {
      return CallRefHint();
    }

    // If we were given no candidates, give up now.
    if (hints.empty()) {
      return CallRefHint();
    }

    // We can't inline if we've exceeded our per-root-function inlining
    // budget.
    //
    // This logic will cause `availableBudget` to be driven slightly negative
    // if a budget overshoot happens, so we will have performed slightly more
    // inlining than allowed by the initial setting of `availableBudget`.  The
    // size of this overshoot is however very limited -- it can't exceed the
    // size of three function bodies that are inlined (3 because that's what
    // CallRefHint can hold).  And the max size of an inlineable function body
    // is limited by InliningHeuristics::isSmallEnoughToInline.
    if (rootCompiler_.inliningBudget() < 0) {
      return CallRefHint();
    }

    // Check each candidate in turn, and add all acceptable ones to `filtered`.
    // It is important that `filtered` retains the same ordering as `hints`.
    CallRefHint filtered;
    for (uint32_t i = 0; i < hints.length(); i++) {
      uint32_t funcIndex = hints.get(i);

      // We can't inline an imported function.
      if (codeMeta().funcIsImport(funcIndex)) {
        continue;
      }

      // We do not support inlining a callee which uses tail calls
      FeatureUsage funcFeatureUsage =
          codeTailMeta()->funcDefFeatureUsage(funcIndex);
      if (funcFeatureUsage & FeatureUsage::ReturnCall) {
        continue;
      }

      // Ask the heuristics system if we're allowed to inline a function of
      // this size and kind at the current inlining depth.
      uint32_t inlineeBodySize = codeTailMeta()->funcDefRange(funcIndex).size();
      uint32_t rootFunctionBodySize = rootCompiler_.func().bytecodeSize();
      bool largeFunctionBackoff;
      bool smallEnough = InliningHeuristics::isSmallEnoughToInline(
          kind, inliningDepth(), inlineeBodySize, rootFunctionBodySize,
          &largeFunctionBackoff);
      if (largeFunctionBackoff) {
        rootCompiler_.noteLargeFunctionBackoffWasApplied();
      }
      if (!smallEnough) {
        continue;
      }

      filtered.append(funcIndex);
    }

    // Whatever ends up in `filtered` is approved for inlining.
    return filtered;
  }

  [[nodiscard]]
  bool callDirect(const FuncType& funcType, uint32_t funcIndex,
                  uint32_t bytecodeOffset, const DefVector& args,
                  DefVector* results) {
    MOZ_ASSERT(!inDeadCode());

    CallCompileState callState(ABIKind::Wasm);
    CallSiteDesc desc(bytecodeOffset, rootCompiler_.inlinedCallerOffsetsIndex(),
                      CallSiteKind::Func);
    ResultType resultType = ResultType::Vector(funcType.results());
    auto callee = CalleeDesc::function(funcIndex);
    ArgTypeVector argTypes(funcType);

    return emitCallArgs(funcType, args, &callState) &&
           call(&callState, desc, callee, argTypes) &&
           collectWasmCallResults(resultType, &callState, results);
  }

  [[nodiscard]]
  bool returnCallDirect(const FuncType& funcType, uint32_t funcIndex,
                        uint32_t bytecodeOffset, const DefVector& args,
                        DefVector* results) {
    MOZ_ASSERT(!inDeadCode());

    // We do not support tail calls in inlined functions.
    MOZ_RELEASE_ASSERT(!isInlined());

    CallCompileState callState(ABIKind::Wasm);
    callState.returnCall = true;
    CallSiteDesc desc(bytecodeOffset, CallSiteKind::ReturnFunc);
    auto callee = CalleeDesc::function(funcIndex);
    ArgTypeVector argTypes(funcType);

    if (!emitCallArgs(funcType, args, &callState)) {
      return false;
    }

    auto ins = MWasmReturnCall::New(
        alloc(), desc, callee, callState.regArgs,
        StackArgAreaSizeUnaligned(argTypes, callState.abiKind), nullptr);
    if (!ins) {
      return false;
    }
    curBlock_->end(ins);
    curBlock_ = nullptr;
    return true;
  }

  [[nodiscard]]
  bool returnCallImport(unsigned globalDataOffset, uint32_t bytecodeOffset,
                        const FuncType& funcType, const DefVector& args,
                        DefVector* results) {
    MOZ_ASSERT(!inDeadCode());

    // We do not support tail calls in inlined functions.
    MOZ_RELEASE_ASSERT(!isInlined());

    CallCompileState callState(ABIKind::Wasm);
    callState.returnCall = true;
    CallSiteDesc desc(bytecodeOffset, CallSiteKind::Import);
    auto callee = CalleeDesc::import(globalDataOffset);
    ArgTypeVector argTypes(funcType);

    if (!emitCallArgs(funcType, args, &callState)) {
      return false;
    }

    auto* ins = MWasmReturnCall::New(
        alloc(), desc, callee, callState.regArgs,
        StackArgAreaSizeUnaligned(argTypes, callState.abiKind), nullptr);
    if (!ins) {
      return false;
    }
    curBlock_->end(ins);
    curBlock_ = nullptr;
    return true;
  }

  [[nodiscard]]
  bool returnCallIndirect(uint32_t funcTypeIndex, uint32_t tableIndex,
                          MDefinition* address, uint32_t bytecodeOffset,
                          const DefVector& args, DefVector* results) {
    MOZ_ASSERT(!inDeadCode());

    // We do not support tail calls in inlined functions.
    MOZ_RELEASE_ASSERT(!isInlined());

    const FuncType& funcType = (*codeMeta().types)[funcTypeIndex].funcType();
    CallIndirectId callIndirectId =
        CallIndirectId::forFuncType(codeMeta(), funcTypeIndex);

    CallCompileState callState(ABIKind::Wasm);
    callState.returnCall = true;
    CalleeDesc callee;
    const TableDesc& table = codeMeta().tables[tableIndex];
    callee =
        CalleeDesc::wasmTable(codeMeta(), table, tableIndex, callIndirectId);

    CallSiteDesc desc(bytecodeOffset, CallSiteKind::Indirect);
    ArgTypeVector argTypes(funcType);

    if (!emitCallArgs(funcType, args, &callState)) {
      return false;
    }

    auto* address32 = boundsCheckAndWrapTableAddress(tableIndex, address);
    if (!address32) {
      return false;
    }

    auto* ins = MWasmReturnCall::New(
        alloc(), desc, callee, callState.regArgs,
        StackArgAreaSizeUnaligned(argTypes, callState.abiKind), address32);
    if (!ins) {
      return false;
    }
    curBlock_->end(ins);
    curBlock_ = nullptr;
    return true;
  }

  [[nodiscard]]
  bool callIndirect(uint32_t funcTypeIndex, uint32_t tableIndex,
                    MDefinition* address, uint32_t lineOrBytecode,
                    const DefVector& args, DefVector* results) {
    MOZ_ASSERT(!inDeadCode());

    CallCompileState callState(ABIKind::Wasm);
    const FuncType& funcType = (*codeMeta().types)[funcTypeIndex].funcType();
    CallIndirectId callIndirectId =
        CallIndirectId::forFuncType(codeMeta(), funcTypeIndex);

    CalleeDesc callee;
    const TableDesc& table = codeMeta().tables[tableIndex];
    callee =
        CalleeDesc::wasmTable(codeMeta(), table, tableIndex, callIndirectId);
    address = boundsCheckAndWrapTableAddress(tableIndex, address);
    if (!address) {
      return false;
    }

    CallSiteDesc desc(lineOrBytecode, rootCompiler_.inlinedCallerOffsetsIndex(),
                      CallSiteKind::Indirect);
    ArgTypeVector argTypes(funcType);
    ResultType resultType = ResultType::Vector(funcType.results());

    return emitCallArgs(funcType, args, &callState) &&
           call(&callState, desc, callee, argTypes, address) &&
           collectWasmCallResults(resultType, &callState, results);
  }

  [[nodiscard]]
  bool callImport(unsigned instanceDataOffset, uint32_t lineOrBytecode,
                  const FuncType& funcType, const DefVector& args,
                  DefVector* results) {
    MOZ_ASSERT(!inDeadCode());

    CallCompileState callState(ABIKind::Wasm);
    CallSiteDesc desc(lineOrBytecode, rootCompiler_.inlinedCallerOffsetsIndex(),
                      CallSiteKind::Import);
    auto callee = CalleeDesc::import(instanceDataOffset);
    ArgTypeVector argTypes(funcType);
    ResultType resultType = ResultType::Vector(funcType.results());

    return emitCallArgs(funcType, args, &callState) &&
           call(&callState, desc, callee, argTypes) &&
           collectWasmCallResults(resultType, &callState, results);
  }

  [[nodiscard]]
  bool builtinCall(CallCompileState* callState,
                   const SymbolicAddressSignature& builtin,
                   uint32_t bytecodeOffset, MDefinition** result) {
    if (inDeadCode()) {
      *result = nullptr;
      return true;
    }

    MOZ_ASSERT(builtin.failureMode == FailureMode::Infallible);

    CallSiteDesc desc(bytecodeOffset, rootCompiler_.inlinedCallerOffsetsIndex(),
                      CallSiteKind::Symbolic);
    auto callee = CalleeDesc::builtin(builtin.identity);

    auto* ins = MWasmCallUncatchable::New(
        alloc(), desc, callee, callState->regArgs,
        StackArgAreaSizeUnaligned(builtin, callState->abiKind));
    if (!ins) {
      return false;
    }
    curBlock_->add(ins);

    return collectBuiltinCallResult(builtin.retType, result, callState);
  }

  [[nodiscard]]
  bool builtinCall1(const SymbolicAddressSignature& builtin,
                    uint32_t bytecodeOffset, MDefinition* arg,
                    MDefinition** result) {
    CallCompileState callState(ABIForBuiltin(builtin.identity));
    return passCallArg(arg, builtin.argTypes[0], &callState) &&
           finishCallArgs(&callState) &&
           builtinCall(&callState, builtin, bytecodeOffset, result);
  }

  [[nodiscard]]
  bool builtinCall2(const SymbolicAddressSignature& builtin,
                    uint32_t bytecodeOffset, MDefinition* arg1,
                    MDefinition* arg2, MDefinition** result) {
    CallCompileState callState(ABIForBuiltin(builtin.identity));
    return passCallArg(arg1, builtin.argTypes[0], &callState) &&
           passCallArg(arg2, builtin.argTypes[1], &callState) &&
           finishCallArgs(&callState) &&
           builtinCall(&callState, builtin, bytecodeOffset, result);
  }

  [[nodiscard]]
  bool builtinCall5(const SymbolicAddressSignature& builtin,
                    uint32_t bytecodeOffset, MDefinition* arg1,
                    MDefinition* arg2, MDefinition* arg3, MDefinition* arg4,
                    MDefinition* arg5, MDefinition** result) {
    CallCompileState callState(ABIForBuiltin(builtin.identity));
    return passCallArg(arg1, builtin.argTypes[0], &callState) &&
           passCallArg(arg2, builtin.argTypes[1], &callState) &&
           passCallArg(arg3, builtin.argTypes[2], &callState) &&
           passCallArg(arg4, builtin.argTypes[3], &callState) &&
           passCallArg(arg5, builtin.argTypes[4], &callState) &&
           finishCallArgs(&callState) &&
           builtinCall(&callState, builtin, bytecodeOffset, result);
  }

  [[nodiscard]]
  bool builtinCall6(const SymbolicAddressSignature& builtin,
                    uint32_t bytecodeOffset, MDefinition* arg1,
                    MDefinition* arg2, MDefinition* arg3, MDefinition* arg4,
                    MDefinition* arg5, MDefinition* arg6,
                    MDefinition** result) {
    CallCompileState callState(ABIForBuiltin(builtin.identity));
    return passCallArg(arg1, builtin.argTypes[0], &callState) &&
           passCallArg(arg2, builtin.argTypes[1], &callState) &&
           passCallArg(arg3, builtin.argTypes[2], &callState) &&
           passCallArg(arg4, builtin.argTypes[3], &callState) &&
           passCallArg(arg5, builtin.argTypes[4], &callState) &&
           passCallArg(arg6, builtin.argTypes[5], &callState) &&
           finishCallArgs(&callState) &&
           builtinCall(&callState, builtin, bytecodeOffset, result);
  }

  [[nodiscard]]
  bool instanceCall(CallCompileState* callState,
                    const SymbolicAddressSignature& builtin,
                    uint32_t bytecodeOffset, MDefinition** result = nullptr) {
    MOZ_ASSERT_IF(!result, builtin.retType == MIRType::None);
    if (inDeadCode()) {
      if (result) {
        *result = nullptr;
      }
      return true;
    }

    CallSiteDesc desc(bytecodeOffset, rootCompiler_.inlinedCallerOffsetsIndex(),
                      CallSiteKind::Symbolic);
    if (builtin.failureMode != FailureMode::Infallible &&
        !beginCatchableCall(callState)) {
      return false;
    }

    MInstruction* ins;
    if (callState->isCatchable()) {
      ins = MWasmCallCatchable::NewBuiltinInstanceMethodCall(
          alloc(), desc, builtin.identity, builtin.failureMode,
          builtin.failureTrap, callState->instanceArg, callState->regArgs,
          StackArgAreaSizeUnaligned(builtin, callState->abiKind),
          callState->tryNoteIndex, callState->fallthroughBlock,
          callState->prePadBlock);
    } else {
      ins = MWasmCallUncatchable::NewBuiltinInstanceMethodCall(
          alloc(), desc, builtin.identity, builtin.failureMode,
          builtin.failureTrap, callState->instanceArg, callState->regArgs,
          StackArgAreaSizeUnaligned(builtin, callState->abiKind));
    }
    if (!ins) {
      return false;
    }
    curBlock_->add(ins);

    if (!finishCatchableCall(callState)) {
      return false;
    }

    if (!result) {
      return true;
    }
    return collectBuiltinCallResult(builtin.retType, result, callState);
  }

  /*********************************************** Instance call helpers ***/

  // Do not call this function directly -- it offers no protection against
  // mis-counting of arguments.  Instead call one of
  // ::emitInstanceCall{0,1,2,3,4,5,6}.
  //
  // Emits a call to the Instance function indicated by `callee`.  This is
  // assumed to take an Instance pointer as its first argument.  The remaining
  // args are taken from `args`, which is assumed to hold `numArgs` entries.
  // If `result` is non-null, the MDefinition* holding the return value is
  // written to `*result`.
  [[nodiscard]]
  bool emitInstanceCallN(uint32_t bytecodeOffset,
                         const SymbolicAddressSignature& callee,
                         MDefinition** args, size_t numArgs,
                         MDefinition** result = nullptr) {
    // Check that the first formal parameter is plausibly an Instance pointer.
    MOZ_ASSERT(callee.numArgs > 0);
    MOZ_ASSERT(callee.argTypes[0] == MIRType::Pointer);
    // Check we agree on the number of args.
    MOZ_ASSERT(numArgs + 1 /* the instance pointer */ == callee.numArgs);
    // Check we agree on whether a value is returned.
    MOZ_ASSERT((result == nullptr) == (callee.retType == MIRType::None));

    // If we are in dead code, it can happen that some of the `args` entries
    // are nullptr, which will look like an OOM to the logic below.  So exit
    // at this point.  `passInstanceCallArg`, `passCallArg`, `finishCall` and
    // `instanceCall` all do nothing in dead code, so it's valid
    // to exit here.
    if (inDeadCode()) {
      if (result) {
        *result = nullptr;
      }
      return true;
    }

    // Check all args for signs of OOMness before attempting to allocating any
    // more memory.
    for (size_t i = 0; i < numArgs; i++) {
      if (!args[i]) {
        if (result) {
          *result = nullptr;
        }
        return false;
      }
    }

    // Finally, construct the call.
    CallCompileState callState(ABIForBuiltin(callee.identity));
    if (!passInstanceCallArg(callee.argTypes[0], &callState)) {
      return false;
    }
    for (size_t i = 0; i < numArgs; i++) {
      if (!passCallArg(args[i], callee.argTypes[i + 1], &callState)) {
        return false;
      }
    }
    if (!finishCallArgs(&callState)) {
      return false;
    }
    return instanceCall(&callState, callee, bytecodeOffset, result);
  }

  [[nodiscard]]
  bool emitInstanceCall0(uint32_t lineOrBytecode,
                         const SymbolicAddressSignature& callee,
                         MDefinition** result = nullptr) {
    MDefinition* args[0] = {};
    return emitInstanceCallN(lineOrBytecode, callee, args, 0, result);
  }
  [[nodiscard]]
  bool emitInstanceCall1(uint32_t lineOrBytecode,
                         const SymbolicAddressSignature& callee,
                         MDefinition* arg1, MDefinition** result = nullptr) {
    MDefinition* args[1] = {arg1};
    return emitInstanceCallN(lineOrBytecode, callee, args, 1, result);
  }
  [[nodiscard]]
  bool emitInstanceCall2(uint32_t lineOrBytecode,
                         const SymbolicAddressSignature& callee,
                         MDefinition* arg1, MDefinition* arg2,
                         MDefinition** result = nullptr) {
    MDefinition* args[2] = {arg1, arg2};
    return emitInstanceCallN(lineOrBytecode, callee, args, 2, result);
  }
  [[nodiscard]]
  bool emitInstanceCall3(uint32_t lineOrBytecode,
                         const SymbolicAddressSignature& callee,
                         MDefinition* arg1, MDefinition* arg2,
                         MDefinition* arg3, MDefinition** result = nullptr) {
    MDefinition* args[3] = {arg1, arg2, arg3};
    return emitInstanceCallN(lineOrBytecode, callee, args, 3, result);
  }
  [[nodiscard]]
  bool emitInstanceCall4(uint32_t lineOrBytecode,
                         const SymbolicAddressSignature& callee,
                         MDefinition* arg1, MDefinition* arg2,
                         MDefinition* arg3, MDefinition* arg4,
                         MDefinition** result = nullptr) {
    MDefinition* args[4] = {arg1, arg2, arg3, arg4};
    return emitInstanceCallN(lineOrBytecode, callee, args, 4, result);
  }
  [[nodiscard]]
  bool emitInstanceCall5(uint32_t lineOrBytecode,
                         const SymbolicAddressSignature& callee,
                         MDefinition* arg1, MDefinition* arg2,
                         MDefinition* arg3, MDefinition* arg4,
                         MDefinition* arg5, MDefinition** result = nullptr) {
    MDefinition* args[5] = {arg1, arg2, arg3, arg4, arg5};
    return emitInstanceCallN(lineOrBytecode, callee, args, 5, result);
  }
  [[nodiscard]]
  bool emitInstanceCall6(uint32_t lineOrBytecode,
                         const SymbolicAddressSignature& callee,
                         MDefinition* arg1, MDefinition* arg2,
                         MDefinition* arg3, MDefinition* arg4,
                         MDefinition* arg5, MDefinition* arg6,
                         MDefinition** result = nullptr) {
    MDefinition* args[6] = {arg1, arg2, arg3, arg4, arg5, arg6};
    return emitInstanceCallN(lineOrBytecode, callee, args, 6, result);
  }

  [[nodiscard]]
  bool callRef(const FuncType& funcType, MDefinition* ref,
               uint32_t lineOrBytecode, const DefVector& args,
               DefVector* results) {
    MOZ_ASSERT(!inDeadCode());

    CallCompileState callState(ABIKind::Wasm);
    CalleeDesc callee = CalleeDesc::wasmFuncRef();
    CallSiteDesc desc(lineOrBytecode, rootCompiler_.inlinedCallerOffsetsIndex(),
                      CallSiteKind::FuncRef);
    ArgTypeVector argTypes(funcType);
    ResultType resultType = ResultType::Vector(funcType.results());

    return emitCallArgs(funcType, args, &callState) &&
           call(&callState, desc, callee, argTypes, ref) &&
           collectWasmCallResults(resultType, &callState, results);
  }

  [[nodiscard]]
  bool returnCallRef(const FuncType& funcType, MDefinition* ref,
                     uint32_t bytecodeOffset, const DefVector& args,
                     DefVector* results) {
    MOZ_ASSERT(!inDeadCode());
    MOZ_ASSERT(!isInlined());

    CallCompileState callState(ABIKind::Wasm);
    callState.returnCall = true;
    CalleeDesc callee = CalleeDesc::wasmFuncRef();
    CallSiteDesc desc(bytecodeOffset, CallSiteKind::FuncRef);
    ArgTypeVector argTypes(funcType);

    if (!emitCallArgs(funcType, args, &callState)) {
      return false;
    }

    auto* ins = MWasmReturnCall::New(
        alloc(), desc, callee, callState.regArgs,
        StackArgAreaSizeUnaligned(argTypes, callState.abiKind), ref);
    if (!ins) {
      return false;
    }
    curBlock_->end(ins);
    curBlock_ = nullptr;
    return true;
  }

  [[nodiscard]] MDefinition* stringCast(MDefinition* string) {
    auto* ins = MWasmTrapIfAnyRefIsNotJSString::New(
        alloc(), string, wasm::Trap::BadCast, trapSiteDesc());
    if (!ins) {
      return ins;
    }
    curBlock_->add(ins);
    return ins;
  }

  [[nodiscard]] MDefinition* stringTest(MDefinition* string) {
    auto* ins = MWasmAnyRefIsJSString::New(alloc(), string);
    if (!ins) {
      return nullptr;
    }
    curBlock_->add(ins);
    return ins;
  }

  [[nodiscard]] MDefinition* stringLength(MDefinition* string) {
    auto* ins = MWasmAnyRefJSStringLength::New(
        alloc(), string, wasm::Trap::BadCast, trapSiteDesc());
    if (!ins) {
      return nullptr;
    }
    curBlock_->add(ins);
    return ins;
  }

  [[nodiscard]] bool dispatchInlineBuiltinModuleFunc(
      const BuiltinModuleFunc& builtinModuleFunc, const DefVector& params) {
    BuiltinInlineOp inlineOp = builtinModuleFunc.inlineOp();
    MOZ_ASSERT(inlineOp != BuiltinInlineOp::None);
    switch (inlineOp) {
      case BuiltinInlineOp::StringCast: {
        MOZ_ASSERT(params.length() == 1);
        MDefinition* string = params[0];
        MDefinition* cast = stringCast(string);
        if (!cast) {
          return false;
        }
        return push(string);
      }
      case BuiltinInlineOp::StringTest: {
        MOZ_ASSERT(params.length() == 1);
        MDefinition* string = params[0];
        MDefinition* test = stringTest(string);
        if (!test) {
          return false;
        }
        return push(test);
      }
      case BuiltinInlineOp::StringLength: {
        MOZ_ASSERT(params.length() == 1);
        MDefinition* string = params[0];
        MDefinition* length = stringLength(string);
        if (!length) {
          return false;
        }
        return push(length);
      }
      case BuiltinInlineOp::None:
      case BuiltinInlineOp::Limit:
        break;
    }
    MOZ_CRASH();
  }

  [[nodiscard]] bool callBuiltinModuleFunc(
      const BuiltinModuleFunc& builtinModuleFunc, const DefVector& params) {
    MOZ_ASSERT(!inDeadCode());

    BuiltinInlineOp inlineOp = builtinModuleFunc.inlineOp();
    if (inlineOp != BuiltinInlineOp::None) {
      return dispatchInlineBuiltinModuleFunc(builtinModuleFunc, params);
    }

    // It's almost possible to use FunctionCompiler::emitInstanceCallN here.
    // Unfortunately not currently possible though, since ::emitInstanceCallN
    // expects an array of arguments along with a size, and that's not what is
    // available here.  It would be possible if we were prepared to copy
    // `builtinModuleFunc->params` into a fixed-sized (16 element?) array, add
    // `memoryBase`, and make the call.
    const SymbolicAddressSignature& callee = *builtinModuleFunc.sig();

    CallCompileState callState(ABIForBuiltin(callee.identity));
    if (!passInstanceCallArg(callee.argTypes[0], &callState) ||
        !passCallArgs(params, builtinModuleFunc.funcType()->args(),
                      &callState)) {
      return false;
    }

    if (builtinModuleFunc.usesMemory()) {
      if (!passCallArg(memoryBase(0), MIRType::Pointer, &callState)) {
        return false;
      }
    }

    if (!finishCallArgs(&callState)) {
      return false;
    }

    bool hasResult = !builtinModuleFunc.funcType()->results().empty();
    MDefinition* result = nullptr;
    MDefinition** resultOutParam = hasResult ? &result : nullptr;
    if (!instanceCall(&callState, callee, readBytecodeOffset(),
                      resultOutParam)) {
      return false;
    }

    if (hasResult) {
      return push(result);
    }
    return true;
  }

  /*********************************************** Control flow generation */

  inline bool inDeadCode() const { return curBlock_ == nullptr; }

  [[nodiscard]] bool returnValues(DefVector&& values) {
    if (inDeadCode()) {
      return true;
    }

    // If we're inlined into another function, we must accumulate the returns
    // so that they can be patched into the caller function.
    if (isInlined()) {
      MGoto* jump = MGoto::New(alloc());
      if (!jump) {
        return false;
      }
      curBlock_->end(jump);
      curBlock_ = nullptr;
      return pendingInlineReturns_.emplaceBack(
          PendingInlineReturn(jump, std::move(values)));
    }

    if (values.empty()) {
      curBlock_->end(MWasmReturnVoid::New(alloc(), instancePointer_));
    } else {
      ResultType resultType = ResultType::Vector(funcType().results());
      ABIResultIter iter(resultType);
      // Switch to iterate in FIFO order instead of the default LIFO.
      while (!iter.done()) {
        iter.next();
      }
      iter.switchToPrev();
      for (uint32_t i = 0; !iter.done(); iter.prev(), i++) {
        if (!mirGen().ensureBallast()) {
          return false;
        }
        const ABIResult& result = iter.cur();
        if (result.onStack()) {
          MOZ_ASSERT(iter.remaining() > 1);
          auto* store = MWasmStoreStackResult::New(
              alloc(), stackResultPointer_, result.stackOffset(), values[i]);
          curBlock_->add(store);
        } else {
          MOZ_ASSERT(iter.remaining() == 1);
          MOZ_ASSERT(i + 1 == values.length());
          curBlock_->end(
              MWasmReturn::New(alloc(), values[i], instancePointer_));
        }
      }
    }
    curBlock_ = nullptr;
    return true;
  }

  void unreachableTrap() {
    if (inDeadCode()) {
      return;
    }

    auto* ins =
        MWasmTrap::New(alloc(), wasm::Trap::Unreachable, trapSiteDesc());
    curBlock_->end(ins);
    curBlock_ = nullptr;
  }

  void unimplementedTrap() {
    if (inDeadCode()) {
      return;
    }

    auto* ins =
        MWasmTrap::New(alloc(), wasm::Trap::Unimplemented, trapSiteDesc());
    curBlock_->end(ins);
    curBlock_ = nullptr;
  }

 private:
  static uint32_t numPushed(MBasicBlock* block) {
    return block->stackDepth() - block->info().firstStackSlot();
  }

 public:
  [[nodiscard]] bool pushDefs(const DefVector& defs) {
    if (inDeadCode()) {
      return true;
    }
    MOZ_ASSERT(numPushed(curBlock_) == 0);
    if (!curBlock_->ensureHasSlots(defs.length())) {
      return false;
    }
    for (MDefinition* def : defs) {
      MOZ_ASSERT(def->type() != MIRType::None);
      curBlock_->push(def);
    }
    return true;
  }

  [[nodiscard]] bool popPushedDefs(DefVector* defs) {
    size_t n = numPushed(curBlock_);
    if (!defs->resizeUninitialized(n)) {
      return false;
    }
    for (; n > 0; n--) {
      MDefinition* def = curBlock_->pop();
      MOZ_ASSERT(def->type() != MIRType::Value);
      (*defs)[n - 1] = def;
    }
    return true;
  }

 private:
  [[nodiscard]] bool addJoinPredecessor(const DefVector& defs,
                                        MBasicBlock** joinPred) {
    *joinPred = curBlock_;
    if (inDeadCode()) {
      return true;
    }
    return pushDefs(defs);
  }

 public:
  [[nodiscard]] bool branchAndStartThen(MDefinition* cond,
                                        MBasicBlock** elseBlock) {
    if (inDeadCode()) {
      *elseBlock = nullptr;
    } else {
      MBasicBlock* thenBlock;
      if (!newBlock(curBlock_, &thenBlock)) {
        return false;
      }
      if (!newBlock(curBlock_, elseBlock)) {
        return false;
      }

      curBlock_->end(MTest::New(alloc(), cond, thenBlock, *elseBlock));

      curBlock_ = thenBlock;
      mirGraph().moveBlockToEnd(curBlock_);
    }

    return startBlock();
  }

  [[nodiscard]] bool switchToElse(MBasicBlock* elseBlock,
                                  MBasicBlock** thenJoinPred) {
    DefVector values;
    if (!finishBlock(&values)) {
      return false;
    }

    if (!elseBlock) {
      *thenJoinPred = nullptr;
    } else {
      if (!addJoinPredecessor(values, thenJoinPred)) {
        return false;
      }

      curBlock_ = elseBlock;
      mirGraph().moveBlockToEnd(curBlock_);
    }

    return startBlock();
  }

  [[nodiscard]] bool joinIfElse(MBasicBlock* thenJoinPred, DefVector* defs) {
    DefVector values;
    if (!finishBlock(&values)) {
      return false;
    }

    if (!thenJoinPred && inDeadCode()) {
      return true;
    }

    MBasicBlock* elseJoinPred;
    if (!addJoinPredecessor(values, &elseJoinPred)) {
      return false;
    }

    mozilla::Array<MBasicBlock*, 2> blocks;
    size_t numJoinPreds = 0;
    if (thenJoinPred) {
      blocks[numJoinPreds++] = thenJoinPred;
    }
    if (elseJoinPred) {
      blocks[numJoinPreds++] = elseJoinPred;
    }

    if (numJoinPreds == 0) {
      return true;
    }

    MBasicBlock* join;
    if (!goToNewBlock(blocks[0], &join)) {
      return false;
    }
    for (size_t i = 1; i < numJoinPreds; ++i) {
      if (!goToExistingBlock(blocks[i], join)) {
        return false;
      }
    }

    curBlock_ = join;
    return popPushedDefs(defs);
  }

  [[nodiscard]] bool startBlock() {
    MOZ_ASSERT_IF(pendingBlockDepth_ < pendingBlocks_.length(),
                  pendingBlocks_[pendingBlockDepth_].patches.empty());
    pendingBlockDepth_++;
    return true;
  }

  [[nodiscard]] bool finishBlock(DefVector* defs) {
    MOZ_ASSERT(pendingBlockDepth_);
    uint32_t topLabel = --pendingBlockDepth_;
    return bindBranches(topLabel, defs);
  }

  [[nodiscard]] bool startLoop(MBasicBlock** loopHeader, size_t paramCount) {
    *loopHeader = nullptr;

    pendingBlockDepth_++;
    rootCompiler_.startLoop();

    if (inDeadCode()) {
      return true;
    }

    // Create the loop header.
    MOZ_ASSERT(curBlock_->loopDepth() == rootCompiler_.loopDepth() - 1);
    *loopHeader = MBasicBlock::New(mirGraph(), info(), curBlock_,
                                   MBasicBlock::PENDING_LOOP_HEADER);
    if (!*loopHeader) {
      return false;
    }

    (*loopHeader)->setLoopDepth(rootCompiler_.loopDepth());
    mirGraph().addBlock(*loopHeader);
    curBlock_->end(MGoto::New(alloc(), *loopHeader));

    DefVector loopParams;
    if (!peekN(paramCount, &loopParams)) {
      return false;
    }

    // Eagerly create a phi for all loop params. setLoopBackedge will remove
    // any that were not necessary.
    for (size_t i = 0; i < paramCount; i++) {
      if (!mirGen().ensureBallast()) {
        return false;
      }
      MPhi* phi = MPhi::New(alloc(), loopParams[i]->type());
      if (!phi) {
        return false;
      }
      if (!phi->reserveLength(2)) {
        return false;
      }
      (*loopHeader)->addPhi(phi);
      phi->addInput(loopParams[i]);
      loopParams[i] = phi;
    }
    pokeN(paramCount, loopParams);

    MBasicBlock* body;
    if (!goToNewBlock(*loopHeader, &body)) {
      return false;
    }
    curBlock_ = body;
    return true;
  }

 private:
  void fixupRedundantPhis(MBasicBlock* b) {
    for (size_t i = 0, depth = b->stackDepth(); i < depth; i++) {
      MDefinition* def = b->getSlot(i);
      if (def->isUnused()) {
        b->setSlot(i, def->toPhi()->getOperand(0));
      }
    }
  }

  [[nodiscard]] bool setLoopBackedge(MBasicBlock* loopEntry,
                                     MBasicBlock* loopBody,
                                     MBasicBlock* backedge, size_t paramCount) {
    if (!loopEntry->setBackedgeWasm(backedge, paramCount)) {
      return false;
    }

    // Entering a loop will eagerly create a phi node for all locals and loop
    // params. Now that we've closed the loop we can check which phi nodes
    // were actually needed by checking if the SSA definition flowing into the
    // loop header (operand 0) is different than the SSA definition coming from
    // the loop backedge (operand 1). If they are the same definition, the phi
    // is redundant and can be removed.
    //
    // To do this we mark all redundant phis as 'unused', then remove the phi's
    // from places in ourself the phis may have flowed into, then replace all
    // uses of the phi's in the MIR graph with the original SSA definition.
    for (MPhiIterator phi = loopEntry->phisBegin(); phi != loopEntry->phisEnd();
         phi++) {
      MOZ_ASSERT(phi->numOperands() == 2);
      if (phi->getOperand(0) == phi->getOperand(1)) {
        phi->setUnused();
      }
    }

    // Fix up phis stored in the slots Vector of pending blocks.
    for (PendingBlockTarget& pendingBlockTarget : pendingBlocks_) {
      for (ControlFlowPatch& p : pendingBlockTarget.patches) {
        MBasicBlock* block = p.ins->block();
        if (block->loopDepth() >= loopEntry->loopDepth()) {
          fixupRedundantPhis(block);
        }
      }
    }

    // The loop body, if any, might be referencing recycled phis too.
    if (loopBody) {
      fixupRedundantPhis(loopBody);
    }

    // Pending jumps to an enclosing try-catch may reference the recycled phis.
    // We have to search above all enclosing try blocks, as a delegate may move
    // patches around.
    for (uint32_t depth = 0; depth < controlStackDepth(); depth++) {
      LabelKind kind = controlKind(depth);
      if (kind != LabelKind::Try && kind != LabelKind::TryTable &&
          kind != LabelKind::Body) {
        continue;
      }
      Control& control = controlItem(depth);
      if (!control.tryControl) {
        continue;
      }
      for (MControlInstruction* patch : control.tryControl->landingPadPatches) {
        MBasicBlock* block = patch->block();
        if (block->loopDepth() >= loopEntry->loopDepth()) {
          fixupRedundantPhis(block);
        }
      }
    }
    for (MControlInstruction* patch : bodyRethrowPadPatches_) {
      MBasicBlock* block = patch->block();
      if (block->loopDepth() >= loopEntry->loopDepth()) {
        fixupRedundantPhis(block);
      }
    }

    // If we're inlined into another function we are accumulating return values
    // in a vector, search through the results to see if any refer to a
    // redundant phi.
    for (PendingInlineReturn& pendingReturn : pendingInlineReturns_) {
      for (uint32_t resultIndex = 0;
           resultIndex < pendingReturn.results.length(); resultIndex++) {
        MDefinition** pendingResult = &pendingReturn.results[resultIndex];
        if ((*pendingResult)->isUnused()) {
          *pendingResult = (*pendingResult)->toPhi()->getOperand(0);
        }
      }
    }

    // Discard redundant phis and add to the free list.
    for (MPhiIterator phi = loopEntry->phisBegin();
         phi != loopEntry->phisEnd();) {
      MPhi* entryDef = *phi++;
      if (!entryDef->isUnused()) {
        continue;
      }

      entryDef->justReplaceAllUsesWith(entryDef->getOperand(0));
      loopEntry->discardPhi(entryDef);
      mirGraph().addPhiToFreeList(entryDef);
    }

    return true;
  }

 public:
  [[nodiscard]] bool closeLoop(MBasicBlock* loopHeader,
                               DefVector* loopResults) {
    MOZ_ASSERT(pendingBlockDepth_ >= 1);
    MOZ_ASSERT(rootCompiler_.loopDepth());

    uint32_t headerLabel = pendingBlockDepth_ - 1;

    if (!loopHeader) {
      MOZ_ASSERT(inDeadCode());
      MOZ_ASSERT(headerLabel >= pendingBlocks_.length() ||
                 pendingBlocks_[headerLabel].patches.empty());
      pendingBlockDepth_--;
      rootCompiler_.closeLoop();
      return true;
    }

    // Op::Loop doesn't have an implicit backedge so temporarily set
    // aside the end of the loop body to bind backedges.
    MBasicBlock* loopBody = curBlock_;
    curBlock_ = nullptr;

    // As explained in bug 1253544, Ion apparently has an invariant that
    // there is only one backedge to loop headers. To handle wasm's ability
    // to have multiple backedges to the same loop header, we bind all those
    // branches as forward jumps to a single backward jump. This is
    // unfortunate but the optimizer is able to fold these into single jumps
    // to backedges.
    DefVector backedgeValues;
    if (!bindBranches(headerLabel, &backedgeValues)) {
      return false;
    }

    MOZ_ASSERT(loopHeader->loopDepth() == rootCompiler_.loopDepth());

    if (curBlock_) {
      // We're on the loop backedge block, created by bindBranches.
      for (size_t i = 0, n = numPushed(curBlock_); i != n; i++) {
        curBlock_->pop();
      }

      if (!pushDefs(backedgeValues)) {
        return false;
      }

      MOZ_ASSERT(curBlock_->loopDepth() == rootCompiler_.loopDepth());
      curBlock_->end(MGoto::New(alloc(), loopHeader));
      if (!setLoopBackedge(loopHeader, loopBody, curBlock_,
                           backedgeValues.length())) {
        return false;
      }
    }

    curBlock_ = loopBody;

    rootCompiler_.closeLoop();

    // If the loop depth still at the inner loop body, correct it.
    if (curBlock_ && curBlock_->loopDepth() != rootCompiler_.loopDepth()) {
      MBasicBlock* out;
      if (!goToNewBlock(curBlock_, &out)) {
        return false;
      }
      curBlock_ = out;
    }

    pendingBlockDepth_ -= 1;
    return inDeadCode() || popPushedDefs(loopResults);
  }

  [[nodiscard]] bool addControlFlowPatch(
      MControlInstruction* ins, uint32_t relative, uint32_t index,
      BranchHint branchHint = BranchHint::Invalid) {
    MOZ_ASSERT(relative < pendingBlockDepth_);
    uint32_t absolute = pendingBlockDepth_ - 1 - relative;

    if (absolute >= pendingBlocks_.length() &&
        !pendingBlocks_.resize(absolute + 1)) {
      return false;
    }

    pendingBlocks_[absolute].hint = branchHint;
    return pendingBlocks_[absolute].patches.append(
        ControlFlowPatch(ins, index));
  }

  [[nodiscard]] bool br(uint32_t relativeDepth, const DefVector& values) {
    if (inDeadCode()) {
      return true;
    }

    MGoto* jump = MGoto::New(alloc());
    if (!addControlFlowPatch(jump, relativeDepth, MGoto::TargetIndex)) {
      return false;
    }

    if (!pushDefs(values)) {
      return false;
    }

    curBlock_->end(jump);
    curBlock_ = nullptr;
    return true;
  }

  [[nodiscard]] bool brIf(uint32_t relativeDepth, const DefVector& values,
                          MDefinition* condition, BranchHint branchHint) {
    if (inDeadCode()) {
      return true;
    }

    MBasicBlock* joinBlock = nullptr;
    if (!newBlock(curBlock_, &joinBlock)) {
      return false;
    }

    MTest* test = MTest::New(alloc(), condition, nullptr, joinBlock);
    if (!addControlFlowPatch(test, relativeDepth, MTest::TrueBranchIndex,
                             branchHint)) {
      return false;
    }

    if (!pushDefs(values)) {
      return false;
    }

    curBlock_->end(test);
    curBlock_ = joinBlock;

    return true;
  }

  [[nodiscard]] bool brTable(MDefinition* operand, uint32_t defaultDepth,
                             const Uint32Vector& depths,
                             const DefVector& values) {
    if (inDeadCode()) {
      return true;
    }

    size_t numCases = depths.length();
    MOZ_ASSERT(numCases <= INT32_MAX);
    MOZ_ASSERT(numCases);

    MTableSwitch* table =
        MTableSwitch::New(alloc(), operand, 0, int32_t(numCases - 1));

    size_t defaultIndex;
    if (!table->addDefault(nullptr, &defaultIndex)) {
      return false;
    }
    if (!addControlFlowPatch(table, defaultDepth, defaultIndex)) {
      return false;
    }

    using IndexToCaseMap =
        HashMap<uint32_t, uint32_t, DefaultHasher<uint32_t>, SystemAllocPolicy>;

    IndexToCaseMap indexToCase;
    if (!indexToCase.put(defaultDepth, defaultIndex)) {
      return false;
    }

    for (size_t i = 0; i < numCases; i++) {
      if (!mirGen().ensureBallast()) {
        return false;
      }

      uint32_t depth = depths[i];

      size_t caseIndex;
      IndexToCaseMap::AddPtr p = indexToCase.lookupForAdd(depth);
      if (!p) {
        if (!table->addSuccessor(nullptr, &caseIndex)) {
          return false;
        }
        if (!addControlFlowPatch(table, depth, caseIndex)) {
          return false;
        }
        if (!indexToCase.add(p, depth, caseIndex)) {
          return false;
        }
      } else {
        caseIndex = p->value();
      }

      if (!table->addCase(caseIndex)) {
        return false;
      }
    }

    if (!pushDefs(values)) {
      return false;
    }

    curBlock_->end(table);
    curBlock_ = nullptr;

    return true;
  }

  /********************************************************** Exceptions ***/

  bool inTryBlockFrom(uint32_t fromRelativeDepth,
                      uint32_t* tryRelativeDepth) const {
    uint32_t relativeDepth;
    if (controlFindInnermostFrom(
            [](LabelKind kind, const Control& control) {
              return control.tryControl != nullptr &&
                     control.tryControl->inBody;
            },
            fromRelativeDepth, &relativeDepth)) {
      *tryRelativeDepth = relativeDepth;
      return true;
    }

    if (callerCompiler_ && callerCompiler_->inTryCode()) {
      *tryRelativeDepth = controlStackDepth() - 1;
      return true;
    }

    return false;
  }

  bool inTryBlockFrom(uint32_t fromRelativeDepth,
                      ControlInstructionVector** landingPadPatches) {
    uint32_t tryRelativeDepth;
    if (!inTryBlockFrom(fromRelativeDepth, &tryRelativeDepth)) {
      return false;
    }

    if (tryRelativeDepth == controlStackDepth() - 1) {
      *landingPadPatches = &bodyRethrowPadPatches_;
    } else {
      *landingPadPatches =
          &controlItem(tryRelativeDepth).tryControl->landingPadPatches;
    }
    return true;
  }

  bool inTryBlock(ControlInstructionVector** landingPadPatches) {
    return inTryBlockFrom(0, landingPadPatches);
  }

  bool inTryCode() const {
    uint32_t tryRelativeDepth;
    return inTryBlockFrom(0, &tryRelativeDepth);
  }

  MDefinition* loadTag(uint32_t tagIndex) {
    MWasmLoadInstanceDataField* tag = MWasmLoadInstanceDataField::New(
        alloc(), MIRType::WasmAnyRef,
        codeMeta().offsetOfTagInstanceData(tagIndex), true, instancePointer_);
    curBlock_->add(tag);
    return tag;
  }

  void loadPendingExceptionState(MDefinition** pendingException,
                                 MDefinition** pendingExceptionTag) {
    auto* exception = MWasmLoadInstance::New(
        alloc(), instancePointer_, wasm::Instance::offsetOfPendingException(),
        MIRType::WasmAnyRef, AliasSet::Load(AliasSet::WasmPendingException));
    curBlock_->add(exception);
    *pendingException = exception;

    auto* tag = MWasmLoadInstance::New(
        alloc(), instancePointer_,
        wasm::Instance::offsetOfPendingExceptionTag(), MIRType::WasmAnyRef,
        AliasSet::Load(AliasSet::WasmPendingException));
    curBlock_->add(tag);
    *pendingExceptionTag = tag;
  }

  [[nodiscard]] bool setPendingExceptionState(MDefinition* exception,
                                              MDefinition* tag) {
    // Set the pending exception object
    auto* exceptionAddr = MWasmDerivedPointer::New(
        alloc(), instancePointer_, Instance::offsetOfPendingException());
    curBlock_->add(exceptionAddr);
    auto* setException = MWasmStoreRef::New(
        alloc(), instancePointer_, exceptionAddr, /*valueOffset=*/0, exception,
        AliasSet::WasmPendingException, WasmPreBarrierKind::Normal);
    curBlock_->add(setException);
    if (!postBarrierEdgePrecise(/*bytecodeOffset=*/0, exceptionAddr,
                                exception)) {
      return false;
    }

    // Set the pending exception tag object
    auto* exceptionTagAddr = MWasmDerivedPointer::New(
        alloc(), instancePointer_, Instance::offsetOfPendingExceptionTag());
    curBlock_->add(exceptionTagAddr);
    auto* setExceptionTag = MWasmStoreRef::New(
        alloc(), instancePointer_, exceptionTagAddr, /*valueOffset=*/0, tag,
        AliasSet::WasmPendingException, WasmPreBarrierKind::Normal);
    curBlock_->add(setExceptionTag);
    return postBarrierEdgePrecise(/*bytecodeOffset=*/0, exceptionTagAddr, tag);
  }

  [[nodiscard]] bool endWithPadPatch(
      ControlInstructionVector* tryLandingPadPatches) {
    MGoto* jumpToLandingPad = MGoto::New(alloc());
    curBlock_->end(jumpToLandingPad);
    return tryLandingPadPatches->emplaceBack(jumpToLandingPad);
  }

  [[nodiscard]] bool delegatePadPatches(const ControlInstructionVector& patches,
                                        uint32_t relativeDepth) {
    if (patches.empty()) {
      return true;
    }

    // Find where we are delegating the pad patches to.
    ControlInstructionVector* targetPatches;
    if (!inTryBlockFrom(relativeDepth, &targetPatches)) {
      MOZ_ASSERT(relativeDepth <= pendingBlockDepth_ - 1);
      targetPatches = &bodyRethrowPadPatches_;
    }

    // Append the delegate's pad patches to the target's.
    for (MControlInstruction* ins : patches) {
      if (!targetPatches->emplaceBack(ins)) {
        return false;
      }
    }
    return true;
  }

  [[nodiscard]]
  bool beginCatchableCall(CallCompileState* callState) {
    if (!inTryBlock(&callState->tryLandingPadPatches)) {
      MOZ_ASSERT(!callState->isCatchable());
      return true;
    }
    MOZ_ASSERT(callState->isCatchable());

    // Allocate a try note
    if (!rootCompiler_.addTryNote(&callState->tryNoteIndex)) {
      return false;
    }

    // Allocate blocks for fallthrough and exceptions
    return newBlock(curBlock_, &callState->fallthroughBlock) &&
           newBlock(curBlock_, &callState->prePadBlock);
  }

  [[nodiscard]]
  bool finishCatchableCall(CallCompileState* callState) {
    if (!callState->tryLandingPadPatches) {
      return true;
    }

    // Switch to the prePadBlock
    MBasicBlock* callBlock = curBlock_;
    curBlock_ = callState->prePadBlock;

    // Mark this as the landing pad for the call
    curBlock_->add(MWasmCallLandingPrePad::New(alloc(), callBlock,
                                               callState->tryNoteIndex));

    // End with a pending jump to the landing pad
    if (!endWithPadPatch(callState->tryLandingPadPatches)) {
      return false;
    }

    // Compilation continues in the fallthroughBlock.
    curBlock_ = callState->fallthroughBlock;
    return true;
  }

  // Create a landing pad for a try block. This is also used for the implicit
  // rethrow landing pad used for delegate instructions that target the
  // outermost label.
  [[nodiscard]]
  bool createTryLandingPad(ControlInstructionVector& landingPadPatches,
                           MBasicBlock** landingPad) {
    MOZ_ASSERT(!landingPadPatches.empty());

    // Bind the branches from exception throwing code to a new landing pad
    // block. This is done similarly to what is done in bindBranches.
    MControlInstruction* ins = landingPadPatches[0];
    MBasicBlock* pred = ins->block();
    if (!newBlock(pred, landingPad)) {
      return false;
    }
    ins->replaceSuccessor(MGoto::TargetIndex, *landingPad);
    for (size_t i = 1; i < landingPadPatches.length(); i++) {
      ins = landingPadPatches[i];
      pred = ins->block();
      if (!(*landingPad)->addPredecessor(alloc(), pred)) {
        return false;
      }
      ins->replaceSuccessor(MGoto::TargetIndex, *landingPad);
    }

    // Clear the now bound pad patches.
    landingPadPatches.clear();
    return true;
  }

  [[nodiscard]]

  // Consume the pending exception state from instance. This will clear out the
  // previous value.
  [[nodiscard]]
  bool consumePendingException(MBasicBlock** landingPad,
                               MDefinition** pendingException,
                               MDefinition** pendingExceptionTag) {
    MBasicBlock* prevBlock = curBlock_;
    curBlock_ = *landingPad;

    // Load the pending exception and tag
    loadPendingExceptionState(pendingException, pendingExceptionTag);

    // Clear the pending exception and tag
    auto* null = constantNullRef(MaybeRefType());
    if (!setPendingExceptionState(null, null)) {
      return false;
    }

    // The landing pad may have changed from loading and clearing the pending
    // exception state.
    *landingPad = curBlock_;

    curBlock_ = prevBlock;
    return true;
  }

  // Finish the previous block (either a try or catch block) and then setup a
  // new catch block.

  [[nodiscard]] bool emitBodyRethrowPad(Control& control) {
    // If there are no throwing instructions pending, we don't need to do
    // anything
    if (bodyRethrowPadPatches_.empty()) {
      return true;
    }

    // Create a landing pad for any throwing instructions
    MBasicBlock* padBlock;
    if (!createTryLandingPad(bodyRethrowPadPatches_, &padBlock)) {
      return false;
    }

    // If we're inlined into another function, we save the landing pad to be
    // linked later directly to our caller's landing pad. See
    // `finishedInlinedCallDirect`.
    if (callerCompiler_ && callerCompiler_->inTryCode()) {
      pendingInlineCatchBlock_ = padBlock;
      return true;
    }

    // Otherwise we need to grab the pending exception and rethrow it.
    MDefinition* pendingException;
    MDefinition* pendingExceptionTag;
    if (!consumePendingException(&padBlock, &pendingException,
                                 &pendingExceptionTag)) {
      return false;
    }

    // Switch to the landing pad and rethrow the exception
    MBasicBlock* prevBlock = curBlock_;
    curBlock_ = padBlock;
    if (!throwFrom(pendingException, pendingExceptionTag)) {
      return false;
    }
    curBlock_ = prevBlock;

    MOZ_ASSERT(bodyRethrowPadPatches_.empty());
    return true;
  }

  [[nodiscard]] bool emitNewException(MDefinition* tag,
                                      MDefinition** exception) {
    return emitInstanceCall1(readBytecodeOffset(), SASigExceptionNew, tag,
                             exception);
  }

  [[nodiscard]] bool throwFrom(MDefinition* exn, MDefinition* tag) {
    if (inDeadCode()) {
      return true;
    }

    // Check if there is a local catching try control, and if so, then add a
    // pad-patch to its tryPadPatches.
    ControlInstructionVector* tryLandingPadPatches;
    if (inTryBlock(&tryLandingPadPatches)) {
      // Set the pending exception state, the landing pad will read from this
      if (!setPendingExceptionState(exn, tag)) {
        return false;
      }

      // End with a pending jump to the landing pad
      if (!endWithPadPatch(tryLandingPadPatches)) {
        return false;
      }
      curBlock_ = nullptr;
      return true;
    }

    // If there is no surrounding catching block, call an instance method to
    // throw the exception.
    if (!emitInstanceCall1(readBytecodeOffset(), SASigThrowException, exn)) {
      return false;
    }
    unreachableTrap();

    curBlock_ = nullptr;
    return true;
  }

  /******************************** WasmGC: low level load/store helpers ***/

  // Given a (StorageType, FieldExtension) pair, produce the (MIRType,
  // MWideningOp) pair that will give the correct operation for reading the
  // value from memory.
  static void fieldLoadInfoToMIR(StorageType type, FieldWideningOp wideningOp,
                                 MIRType* mirType, MWideningOp* mirWideningOp) {
    switch (type.kind()) {
      case StorageType::I8: {
        switch (wideningOp) {
          case FieldWideningOp::Signed:
            *mirType = MIRType::Int32;
            *mirWideningOp = MWideningOp::FromS8;
            return;
          case FieldWideningOp::Unsigned:
            *mirType = MIRType::Int32;
            *mirWideningOp = MWideningOp::FromU8;
            return;
          default:
            MOZ_CRASH();
        }
      }
      case StorageType::I16: {
        switch (wideningOp) {
          case FieldWideningOp::Signed:
            *mirType = MIRType::Int32;
            *mirWideningOp = MWideningOp::FromS16;
            return;
          case FieldWideningOp::Unsigned:
            *mirType = MIRType::Int32;
            *mirWideningOp = MWideningOp::FromU16;
            return;
          default:
            MOZ_CRASH();
        }
      }
      default: {
        switch (wideningOp) {
          case FieldWideningOp::None:
            *mirType = type.toMIRType();
            *mirWideningOp = MWideningOp::None;
            return;
          default:
            MOZ_CRASH();
        }
      }
    }
  }

  // Given a StorageType, return the Scale required when accessing array
  // elements of this type.
  static Scale scaleFromFieldType(StorageType type) {
    if (type.kind() == StorageType::V128) {
      // V128 is accessed differently, so this scale will not be used.
      return Scale::Invalid;
    }
    return ShiftToScale(type.indexingShift());
  }

  // Given a StorageType, produce the MNarrowingOp required for writing the
  // value to memory.
  static MNarrowingOp fieldStoreInfoToMIR(StorageType type) {
    switch (type.kind()) {
      case StorageType::I8:
        return MNarrowingOp::To8;
      case StorageType::I16:
        return MNarrowingOp::To16;
      default:
        return MNarrowingOp::None;
    }
  }

  // Generate a write of `value` at address `base + offset`, where `offset` is
  // known at JIT time.  If the written value is a reftype, the previous value
  // at `base + offset` will be retrieved and handed off to the post-write
  // barrier.  `keepAlive` will be referenced by the instruction so as to hold
  // it live (from the GC's point of view).
  [[nodiscard]] bool writeGcValueAtBasePlusOffset(
      uint32_t bytecodeOffset, StorageType type, MDefinition* keepAlive,
      AliasSet::Flag aliasBitset, MDefinition* value, MDefinition* base,
      uint32_t offset, uint32_t fieldIndex, bool needsTrapInfo,
      WasmPreBarrierKind preBarrierKind, WasmPostBarrierKind postBarrierKind) {
    MOZ_ASSERT(aliasBitset != 0);
    MOZ_ASSERT(keepAlive->type() == MIRType::WasmAnyRef);
    MOZ_ASSERT(type.widenToValType().toMIRType() == value->type());
    MNarrowingOp narrowingOp = fieldStoreInfoToMIR(type);

    if (!type.isRefRepr()) {
      MaybeTrapSiteDesc maybeTrap;
      if (needsTrapInfo) {
        maybeTrap.emplace(trapSiteDesc());
      }

      auto* store = MWasmStoreField::New(
          alloc(), base, keepAlive, offset, mozilla::Some(fieldIndex), value,
          narrowingOp, AliasSet::Store(aliasBitset), maybeTrap);
      if (!store) {
        return false;
      }
      curBlock_->add(store);
      return true;
    }

    // Otherwise it's a ref store.  Load the previous value so we can show it
    // to the post-write barrier.
    //
    // Optimisation opportunity: for the case where this field write results
    // from struct.new, the old value is always zero.  So we should synthesise
    // a suitable zero constant rather than reading it from the object.  See
    // also bug 1799999.
    MOZ_ASSERT(narrowingOp == MNarrowingOp::None);
    MOZ_ASSERT(type.widenToValType() == type.valType());

    // Store the new value
    auto* store = MWasmStoreFieldRef::New(
        alloc(), instancePointer_, base, keepAlive, offset,
        mozilla::Some(fieldIndex), value, AliasSet::Store(aliasBitset),
        mozilla::Some(trapSiteDesc()), preBarrierKind);
    if (!store) {
      return false;
    }
    curBlock_->add(store);

    // Call the post-write barrier
    switch (postBarrierKind) {
      case WasmPostBarrierKind::WholeCell:
        return postBarrierWholeCell(bytecodeOffset, keepAlive, value);
      case WasmPostBarrierKind::Edge:
        MOZ_CRASH("WasmPostBarrierKind::Edge not supported");
      case WasmPostBarrierKind::None:
        return true;
      default:
        MOZ_CRASH("Bad postBarrierKind");
    }
  }

  // Generate a write of `value` at address `base + index * scale`, where
  // `scale` is known at JIT-time.  If the written value is a reftype, the
  // previous value at `base + index * scale` will be retrieved and handed off
  // to the post-write barrier.  `keepAlive` will be referenced by the
  // instruction so as to hold it live (from the GC's point of view).
  [[nodiscard]] bool writeGcValueAtBasePlusScaledIndex(
      uint32_t bytecodeOffset, StorageType type, MDefinition* keepAlive,
      AliasSet::Flag aliasBitset, MDefinition* value, MDefinition* base,
      uint32_t scale, MDefinition* index, WasmPreBarrierKind preBarrierKind,
      WasmPostBarrierKind postBarrierKind) {
    MOZ_ASSERT(aliasBitset != 0);
    MOZ_ASSERT(keepAlive->type() == MIRType::WasmAnyRef);
    MOZ_ASSERT(type.widenToValType().toMIRType() == value->type());
    MOZ_ASSERT(scale == 1 || scale == 2 || scale == 4 || scale == 8 ||
               scale == 16);

    MNarrowingOp narrowingOp = fieldStoreInfoToMIR(type);

    if (!type.isRefRepr()) {
      MaybeTrapSiteDesc maybeTrap;
      Scale scale = scaleFromFieldType(type);
      auto* store = MWasmStoreElement::New(
          alloc(), base, index, value, keepAlive, narrowingOp, scale,
          AliasSet::Store(aliasBitset), maybeTrap);
      if (!store) {
        return false;
      }
      curBlock_->add(store);
      return true;
    }

    // Otherwise it's a ref store.
    MOZ_ASSERT(narrowingOp == MNarrowingOp::None);
    MOZ_ASSERT(type.widenToValType() == type.valType());

    // Store the new value
    auto* store = MWasmStoreElementRef::New(
        alloc(), instancePointer_, base, index, value, keepAlive,
        AliasSet::Store(aliasBitset), mozilla::Some(trapSiteDesc()),
        preBarrierKind);
    if (!store) {
      return false;
    }
    curBlock_->add(store);

    switch (postBarrierKind) {
      case WasmPostBarrierKind::WholeCell:
        return postBarrierWholeCell(bytecodeOffset, keepAlive, value);
      case WasmPostBarrierKind::Edge:
        return postBarrierEdgeAtIndex(bytecodeOffset, keepAlive, base, index,
                                      sizeof(void*), value);
      case WasmPostBarrierKind::None:
        return true;
      default:
        MOZ_CRASH("Bad postBarrierKind");
    }
  }

  // Generate a read from address `base + offset`, where `offset` is known at
  // JIT time.  The loaded value will be widened as described by `type` and
  // `fieldWideningOp`.  `keepAlive` will be referenced by the instruction so as
  // to hold it live (from the GC's point of view).
  [[nodiscard]] MDefinition* readGcValueAtBasePlusOffset(
      StorageType type, FieldWideningOp fieldWideningOp, MDefinition* keepAlive,
      AliasSet::Flag aliasBitset, MDefinition* base, uint32_t offset,
      uint32_t fieldIndex, bool needsTrapInfo) {
    MOZ_ASSERT(aliasBitset != 0);
    MOZ_ASSERT(keepAlive->type() == MIRType::WasmAnyRef);
    MIRType mirType;
    MWideningOp mirWideningOp;
    fieldLoadInfoToMIR(type, fieldWideningOp, &mirType, &mirWideningOp);
    MaybeTrapSiteDesc maybeTrap;
    if (needsTrapInfo) {
      maybeTrap.emplace(trapSiteDesc());
    }

    auto* load = MWasmLoadField::New(alloc(), base, keepAlive, offset,
                                     mozilla::Some(fieldIndex), mirType,
                                     mirWideningOp, AliasSet::Load(aliasBitset),
                                     maybeTrap, type.toMaybeRefType());
    if (!load) {
      return nullptr;
    }
    curBlock_->add(load);
    return load;
  }

  // Generate a read from address `base + index * scale`, where `scale` is
  // known at JIT-time.  The loaded value will be widened as described by
  // `type` and `fieldWideningOp`.  `keepAlive` will be referenced by the
  // instruction so as to hold it live (from the GC's point of view).
  [[nodiscard]] MDefinition* readGcArrayValueAtIndex(
      StorageType type, FieldWideningOp fieldWideningOp, MDefinition* keepAlive,
      AliasSet::Flag aliasBitset, MDefinition* base, MDefinition* index) {
    MOZ_ASSERT(aliasBitset != 0);
    MOZ_ASSERT(keepAlive->type() == MIRType::WasmAnyRef);

    MIRType mirType;
    MWideningOp mirWideningOp;
    fieldLoadInfoToMIR(type, fieldWideningOp, &mirType, &mirWideningOp);
    Scale scale = scaleFromFieldType(type);
    auto* load = MWasmLoadElement::New(
        alloc(), base, keepAlive, index, mirType, mirWideningOp, scale,
        AliasSet::Load(aliasBitset), mozilla::Some(trapSiteDesc()),
        type.toMaybeRefType());
    if (!load) {
      return nullptr;
    }
    curBlock_->add(load);
    return load;
  }

  /************************************************ WasmGC: type helpers ***/

  // Returns an MDefinition holding the supertype vector for `typeIndex`.
  [[nodiscard]] MDefinition* loadSuperTypeVector(uint32_t typeIndex) {
    uint32_t stvOffset = codeMeta().offsetOfSuperTypeVector(typeIndex);

    auto* load =
        MWasmLoadInstanceDataField::New(alloc(), MIRType::Pointer, stvOffset,
                                        /*isConst=*/true, instancePointer_);
    if (!load) {
      return nullptr;
    }
    curBlock_->add(load);
    return load;
  }

  uint32_t readAllocSiteIndex(uint32_t typeIndex) {
    if (!codeTailMeta() || !codeTailMeta()->hasFuncDefAllocSites()) {
      // For single tier of optimized compilation, there are no assigned alloc
      // sites, using type index as alloc site.
      return typeIndex;
    }
    AllocSitesRange rangeInModule =
        codeTailMeta()->getFuncDefAllocSites(funcIndex());
    uint32_t localIndex = numAllocSites_++;
    MOZ_RELEASE_ASSERT(localIndex < rangeInModule.length);
    return rangeInModule.begin + localIndex;
  }

  [[nodiscard]] MDefinition* loadAllocSiteInstanceData(
      uint32_t allocSiteIndex) {
    auto* allocSites = MWasmLoadInstance::New(
        alloc(), instancePointer_, Instance::offsetOfAllocSites(),
        MIRType::Pointer, AliasSet::None());
    if (!allocSites) {
      return nullptr;
    }
    curBlock_->add(allocSites);

    auto* result = MWasmDerivedPointer::New(
        alloc(), allocSites, allocSiteIndex * sizeof(gc::AllocSite));
    if (!result) {
      return nullptr;
    }
    curBlock_->add(result);
    return result;
  }

  /********************************************** WasmGC: struct helpers ***/

  [[nodiscard]] MDefinition* createStructObject(uint32_t typeIndex,
                                                uint32_t allocSiteIndex,
                                                bool zeroFields) {
    // Allocate an uninitialized struct.
    MDefinition* allocSite = loadAllocSiteInstanceData(allocSiteIndex);
    if (!allocSite) {
      return nullptr;
    }

    const TypeDef* typeDef = &(*codeMeta().types)[typeIndex];
    auto* structObject =
        MWasmNewStructObject::New(alloc(), instancePointer_, allocSite, typeDef,
                                  zeroFields, trapSiteDesc());
    if (!structObject) {
      return nullptr;
    }
    curBlock_->add(structObject);

    return structObject;
  }

  // Helper function for EmitStruct{New,Set}: given a MIR pointer to a
  // WasmStructObject, a MIR pointer to a value, and a field descriptor,
  // generate MIR to write the value to the relevant field in the object.
  [[nodiscard]] bool writeValueToStructField(
      uint32_t bytecodeOffset, const StructType& structType,
      uint32_t fieldIndex, MDefinition* structObject, MDefinition* value,
      WasmPreBarrierKind preBarrierKind) {
    StorageType fieldType = structType.fields_[fieldIndex].type;
    FieldAccessPath path = structType.fieldAccessPaths_[fieldIndex];
    uint32_t areaOffset = path.hasOOL() ? path.oolOffset() : path.ilOffset();

    // Make `base` point at the first byte of either the struct object as a
    // whole or of the out-of-line data area.
    MDefinition* base;
    bool needsTrapInfo;
    if (path.hasOOL()) {
      // The path has two components, of which the first (the IL component) is
      // the offset where the OOL pointer is stored.  Hence `path.ilOffset()`.
      auto* loadDataPointer = MWasmLoadField::New(
          alloc(), structObject, nullptr, path.ilOffset(), mozilla::Nothing(),
          MIRType::WasmStructData, MWideningOp::None,
          AliasSet::Load(AliasSet::WasmStructOutlineDataPointer),
          mozilla::Some(trapSiteDesc()));
      if (!loadDataPointer) {
        return false;
      }
      curBlock_->add(loadDataPointer);
      base = loadDataPointer;
      needsTrapInfo = false;
    } else {
      base = structObject;
      needsTrapInfo = true;
    }
    // The transaction is to happen at `base + areaOffset`, so to speak.

    // The alias set denoting the field's location, although lacking a
    // Load-vs-Store indication at this point.
    AliasSet::Flag fieldAliasSet = path.hasOOL()
                                       ? AliasSet::WasmStructOutlineDataArea
                                       : AliasSet::WasmStructInlineDataArea;

    return writeGcValueAtBasePlusOffset(
        bytecodeOffset, fieldType, structObject, fieldAliasSet, value, base,
        areaOffset, fieldIndex, needsTrapInfo, preBarrierKind,
        WasmPostBarrierKind::WholeCell);
  }

  // Helper function for EmitStructGet: given a MIR pointer to a
  // WasmStructObject, a field descriptor and a field widening operation,
  // generate MIR to read the value from the relevant field in the object.
  [[nodiscard]] MDefinition* readValueFromStructField(
      const StructType& structType, uint32_t fieldIndex,
      FieldWideningOp wideningOp, MDefinition* structObject) {
    StorageType fieldType = structType.fields_[fieldIndex].type;
    FieldAccessPath path = structType.fieldAccessPaths_[fieldIndex];
    uint32_t areaOffset = path.hasOOL() ? path.oolOffset() : path.ilOffset();

    // Make `base` point at the first byte of either the struct object as a
    // whole or of the out-of-line data area.
    MDefinition* base;
    bool needsTrapInfo;
    if (path.hasOOL()) {
      // The path has two components, of which the first (the IL component) is
      // the offset where the OOL pointer is stored.  Hence `path.ilOffset()`.
      auto* loadDataPointer = MWasmLoadField::New(
          alloc(), structObject, nullptr, path.ilOffset(), mozilla::Nothing(),
          MIRType::WasmStructData, MWideningOp::None,
          AliasSet::Load(AliasSet::WasmStructOutlineDataPointer),
          mozilla::Some(trapSiteDesc()));
      if (!loadDataPointer) {
        return nullptr;
      }
      curBlock_->add(loadDataPointer);
      base = loadDataPointer;
      needsTrapInfo = false;
    } else {
      base = structObject;
      needsTrapInfo = true;
    }
    // The transaction is to happen at `base + areaOffset`, so to speak.

    // The alias set denoting the field's location, although lacking a
    // Load-vs-Store indication at this point.
    AliasSet::Flag fieldAliasSet = path.hasOOL()
                                       ? AliasSet::WasmStructOutlineDataArea
                                       : AliasSet::WasmStructInlineDataArea;

    return readGcValueAtBasePlusOffset(fieldType, wideningOp, structObject,
                                       fieldAliasSet, base, areaOffset,
                                       fieldIndex, needsTrapInfo);
  }

  /********************************* WasmGC: address-arithmetic helpers ***/

  inline bool targetIs64Bit() const {
#ifdef JS_64BIT
    return true;
#else
    return false;
#endif
  }

  // Generate MIR to unsigned widen `val` out to the target word size.  If
  // `val` is already at the target word size, this is a no-op.  The only
  // other allowed case is where `val` is Int32 and we're compiling for a
  // 64-bit target, in which case a widen is generated.
  [[nodiscard]] MDefinition* unsignedWidenToTargetWord(MDefinition* val) {
    if (targetIs64Bit()) {
      if (val->type() == MIRType::Int32) {
        auto* ext = MExtendInt32ToInt64::New(alloc(), val, /*isUnsigned=*/true);
        if (!ext) {
          return nullptr;
        }
        curBlock_->add(ext);
        return ext;
      }
      MOZ_ASSERT(val->type() == MIRType::Int64);
      return val;
    }
    MOZ_ASSERT(val->type() == MIRType::Int32);
    return val;
  }

  /********************************************** WasmGC: array helpers ***/

  // Given `arrayObject`, the address of a WasmArrayObject, generate MIR to
  // return the contents of the WasmArrayObject::numElements_ field.
  // Adds trap site info for the null check.
  [[nodiscard]] MDefinition* getWasmArrayObjectNumElements(
      MDefinition* arrayObject) {
    MOZ_ASSERT(arrayObject->type() == MIRType::WasmAnyRef);

    auto* numElements = MWasmLoadField::New(
        alloc(), arrayObject, nullptr, WasmArrayObject::offsetOfNumElements(),
        mozilla::Nothing(), MIRType::Int32, MWideningOp::None,
        AliasSet::Load(AliasSet::WasmArrayNumElements),
        mozilla::Some(trapSiteDesc()));
    if (!numElements) {
      return nullptr;
    }
    curBlock_->add(numElements);

    return numElements;
  }

  // Given `arrayObject`, the address of a WasmArrayObject, generate MIR to
  // return the contents of the WasmArrayObject::data_ field.
  [[nodiscard]] MDefinition* getWasmArrayObjectData(MDefinition* arrayObject) {
    MOZ_ASSERT(arrayObject->type() == MIRType::WasmAnyRef);

    auto* data = MWasmLoadField::New(
        alloc(), arrayObject, nullptr, WasmArrayObject::offsetOfData(),
        mozilla::Nothing(), MIRType::WasmArrayData, MWideningOp::None,
        AliasSet::Load(AliasSet::WasmArrayDataPointer),
        mozilla::Some(trapSiteDesc()));
    if (!data) {
      return nullptr;
    }
    curBlock_->add(data);

    return data;
  }

  // Given a JIT-time-known type index `typeIndex` and a run-time known number
  // of elements `numElements`, create MIR to allocate a new wasm array,
  // possibly initialized with `typeIndex`s default value.
  [[nodiscard]] MDefinition* createArrayObject(uint32_t typeIndex,
                                               uint32_t allocSiteIndex,
                                               MDefinition* numElements,
                                               bool zeroFields) {
    MDefinition* allocSite = loadAllocSiteInstanceData(allocSiteIndex);
    if (!allocSite) {
      return nullptr;
    }

    const TypeDef* typeDef = &(*codeMeta().types)[typeIndex];
    auto* arrayObject = MWasmNewArrayObject::New(
        alloc(), instancePointer_, numElements, allocSite, typeDef, zeroFields,
        trapSiteDesc());
    if (!arrayObject) {
      return nullptr;
    }
    curBlock_->add(arrayObject);

    return arrayObject;
  }

  // This emits MIR to perform several actions common to array loads and
  // stores.  Given `arrayObject`, that points to a WasmArrayObject, and an
  // index value `index`, it:
  //
  // * Generates a trap if the array pointer is null
  // * Gets the size of the array
  // * Emits a bounds check of `index` against the array size
  // * Retrieves the OOL object pointer from the array
  // * Includes check for null via signal handler.
  //
  // The returned value is for the OOL object pointer.
  [[nodiscard]] MDefinition* setupForArrayAccess(MDefinition* arrayObject,
                                                 MDefinition* index) {
    MOZ_ASSERT(arrayObject->type() == MIRType::WasmAnyRef);
    MOZ_ASSERT(index->type() == MIRType::Int32);

    // Check for null is done in getWasmArrayObjectNumElements.

    // Get the size value for the array.
    MDefinition* numElements = getWasmArrayObjectNumElements(arrayObject);
    if (!numElements) {
      return nullptr;
    }

    // Create a bounds check.
    auto* boundsCheck =
        MWasmBoundsCheck::New(alloc(), index, numElements, trapSiteDesc(),
                              MWasmBoundsCheck::Target::Other);
    if (!boundsCheck) {
      return nullptr;
    }
    curBlock_->add(boundsCheck);

    // Get the address of the first byte of the (OOL) data area.
    return getWasmArrayObjectData(arrayObject);
  }

  [[nodiscard]] bool fillArray(uint32_t bytecodeOffset,
                               const ArrayType& arrayType,
                               MDefinition* arrayObject, MDefinition* index,
                               MDefinition* numElements, MDefinition* val,
                               WasmPreBarrierKind preBarrierKind,
                               WasmPostBarrierKind postBarrierKind) {
    mozilla::DebugOnly<MIRType> valMIRType = val->type();
    StorageType elemType = arrayType.elementType();
    MOZ_ASSERT(elemType.widenToValType().toMIRType() == valMIRType);

    uint32_t elemSize = elemType.size();
    MOZ_ASSERT(elemSize >= 1 && elemSize <= 16);

    // Make `arrayBase` point at the first byte of the (OOL) data area.
    MDefinition* arrayBase = getWasmArrayObjectData(arrayObject);
    if (!arrayBase) {
      return false;
    }

    // We have:
    //   arrayBase   : TargetWord
    //   index       : Int32
    //   numElements : Int32
    //   val         : <any StorageType>
    //   $elemSize = arrayType.elementType_.size(); 1, 2, 4, 8 or 16
    //
    // Generate MIR:
    //   <in current block>
    //     limit : Int32 = index + numElements
    //     if (limit == index) goto after; // skip loop if trip count == 0
    //   loop:
    //     indexPhi = phi(index, indexNext)
    //     arrayBase[index * $elemSize] = val
    //     indexNext = indexPhi + 1
    //     if (indexNext <u limit) goto loop;
    //   after:
    //
    // We construct the loop "manually" rather than using
    // FunctionCompiler::{startLoop,closeLoop} as the latter have awareness of
    // the wasm view of loops, whereas the loop we're building here is not a
    // wasm-level loop.
    // ==== Create the "loop" and "after" blocks ====
    MBasicBlock* loopBlock;
    if (!newBlock(curBlock_, &loopBlock, MBasicBlock::LOOP_HEADER)) {
      return false;
    }
    MBasicBlock* afterBlock;
    if (!newBlock(loopBlock, &afterBlock)) {
      return false;
    }

    // ==== Fill in the remainder of the block preceding the loop ====
    MAdd* limit = MAdd::NewWasm(alloc(), index, numElements, MIRType::Int32);
    if (!limit) {
      return false;
    }
    curBlock_->add(limit);

    // Note: the comparison (and eventually the entire initialisation loop) will
    // be folded out in the case where the number of elements is zero.
    // See MCompare::tryFoldEqualOperands.
    MDefinition* limitEqualsBase =
        compare(limit, index, JSOp::StrictEq, MCompare::Compare_UInt32);
    if (!limitEqualsBase) {
      return false;
    }
    MTest* skipIfLimitEqualsBase =
        MTest::New(alloc(), limitEqualsBase, afterBlock, loopBlock);
    if (!skipIfLimitEqualsBase) {
      return false;
    }
    curBlock_->end(skipIfLimitEqualsBase);
    if (!afterBlock->addPredecessor(alloc(), curBlock_)) {
      return false;
    }

    // ==== Fill in the loop block as best we can ====
    curBlock_ = loopBlock;
    MPhi* indexPhi = MPhi::New(alloc(), MIRType::Int32);
    if (!indexPhi) {
      return false;
    }
    if (!indexPhi->reserveLength(2)) {
      return false;
    }
    indexPhi->addInput(index);
    curBlock_->addPhi(indexPhi);
    curBlock_->setLoopDepth(rootCompiler_.loopDepth() + 1);

    if (!writeGcValueAtBasePlusScaledIndex(
            bytecodeOffset, elemType, arrayObject, AliasSet::WasmArrayDataArea,
            val, arrayBase, elemSize, indexPhi, preBarrierKind,
            postBarrierKind)) {
      return false;
    }

    auto* indexNext =
        MAdd::NewWasm(alloc(), indexPhi, constantI32(1), MIRType::Int32);
    if (!indexNext) {
      return false;
    }
    curBlock_->add(indexNext);
    indexPhi->addInput(indexNext);

    MDefinition* indexNextLtuLimit =
        compare(indexNext, limit, JSOp::Lt, MCompare::Compare_UInt32);
    if (!indexNextLtuLimit) {
      return false;
    }
    auto* continueIfIndexNextLtuLimit =
        MTest::New(alloc(), indexNextLtuLimit, loopBlock, afterBlock);
    if (!continueIfIndexNextLtuLimit) {
      return false;
    }
    curBlock_->end(continueIfIndexNextLtuLimit);
    if (!loopBlock->addPredecessor(alloc(), loopBlock)) {
      return false;
    }
    // ==== Loop block completed ====

    curBlock_ = afterBlock;
    return true;
  }

  [[nodiscard]] bool createArrayCopy(uint32_t bytecodeOffset,
                                     MDefinition* dstArrayObject,
                                     MDefinition* dstArrayIndex,
                                     MDefinition* srcArrayObject,
                                     MDefinition* srcArrayIndex,
                                     MDefinition* numElements, int32_t elemSize,
                                     bool elemsAreRefTyped) {
    // Check for null is done in getWasmArrayObjectNumElements.

    // Get the arrays' actual sizes.
    MDefinition* dstNumElements = getWasmArrayObjectNumElements(dstArrayObject);
    if (!dstNumElements) {
      return false;
    }
    MDefinition* srcNumElements = getWasmArrayObjectNumElements(srcArrayObject);
    if (!srcNumElements) {
      return false;
    }

    // Create the bounds checks.
    MInstruction* dstBoundsCheck = MWasmBoundsCheckRange32::New(
        alloc(), dstArrayIndex, numElements, dstNumElements, trapSiteDesc());
    if (!dstBoundsCheck) {
      return false;
    }
    curBlock_->add(dstBoundsCheck);

    MInstruction* srcBoundsCheck = MWasmBoundsCheckRange32::New(
        alloc(), srcArrayIndex, numElements, srcNumElements, trapSiteDesc());
    if (!srcBoundsCheck) {
      return false;
    }
    curBlock_->add(srcBoundsCheck);

    // Check if numElements != 0 -- optimization to not invoke builtins.
    MBasicBlock* copyBlock = nullptr;
    if (!newBlock(curBlock_, &copyBlock)) {
      return false;
    }
    MBasicBlock* joinBlock = nullptr;
    if (!newBlock(curBlock_, &joinBlock)) {
      return false;
    }

    MInstruction* condition =
        MCompare::NewWasm(alloc(), numElements, constantI32(0), JSOp::StrictEq,
                          MCompare::Compare_UInt32);
    curBlock_->add(condition);

    MTest* test = MTest::New(alloc(), condition, joinBlock, copyBlock);
    if (!test) {
      return false;
    }
    curBlock_->end(test);
    curBlock_ = copyBlock;

    if (elemsAreRefTyped) {
      MOZ_RELEASE_ASSERT(elemSize == sizeof(void*));

      if (!builtinCall5(SASigArrayRefsMove, bytecodeOffset, dstArrayObject,
                        dstArrayIndex, srcArrayObject, srcArrayIndex,
                        numElements, nullptr)) {
        return false;
      }
    } else {
      MDefinition* elemSizeDef = constantI32(elemSize);
      if (!elemSizeDef) {
        return false;
      }

      if (!builtinCall6(SASigArrayMemMove, bytecodeOffset, dstArrayObject,
                        dstArrayIndex, srcArrayObject, srcArrayIndex,
                        elemSizeDef, numElements, nullptr)) {
        return false;
      }
    }

    MGoto* fallthrough = MGoto::New(alloc(), joinBlock);
    if (!fallthrough) {
      return false;
    }
    curBlock_->end(fallthrough);
    if (!joinBlock->addPredecessor(alloc(), curBlock_)) {
      return false;
    }
    curBlock_ = joinBlock;
    return true;
  }

  [[nodiscard]] bool createArrayFill(uint32_t bytecodeOffset,
                                     uint32_t typeIndex,
                                     MDefinition* arrayObject,
                                     MDefinition* index, MDefinition* val,
                                     MDefinition* numElements) {
    MOZ_ASSERT(arrayObject->type() == MIRType::WasmAnyRef);
    MOZ_ASSERT(index->type() == MIRType::Int32);
    MOZ_ASSERT(numElements->type() == MIRType::Int32);

    const ArrayType& arrayType = (*codeMeta().types)[typeIndex].arrayType();

    // Check for null is done in getWasmArrayObjectNumElements.

    // Get the array's actual size.
    MDefinition* actualNumElements = getWasmArrayObjectNumElements(arrayObject);
    if (!actualNumElements) {
      return false;
    }

    // Create a bounds check.
    auto* boundsCheck = MWasmBoundsCheckRange32::New(
        alloc(), index, numElements, actualNumElements, trapSiteDesc());
    if (!boundsCheck) {
      return false;
    }
    curBlock_->add(boundsCheck);

    return fillArray(bytecodeOffset, arrayType, arrayObject, index, numElements,
                     val, WasmPreBarrierKind::Normal,
                     WasmPostBarrierKind::Edge);
  }

  /*********************************************** WasmGC: other helpers ***/

  // Generate MIR that attempts to cast `ref` to `castToTypeDef`.  If the
  // cast fails, we trap.  If it succeeds, then `ref` can be assumed to
  // have a type that is a subtype of (or the same as) `castToTypeDef` after
  // this point.
  [[nodiscard]] MDefinition* refCast(MDefinition* ref, RefType destType) {
    MInstruction* cast = nullptr;
    if (destType.isTypeRef()) {
      uint32_t typeIndex = codeMeta().types->indexOf(*destType.typeDef());
      MDefinition* superSTV = loadSuperTypeVector(typeIndex);
      if (!superSTV) {
        return nullptr;
      }
      cast = MWasmRefCastConcrete::New(alloc(), ref, superSTV, destType,
                                       trapSiteDesc());
    } else {
      cast = MWasmRefCastAbstract::New(alloc(), ref, destType, trapSiteDesc());
    }

    if (!cast) {
      return nullptr;
    }
    curBlock_->add(cast);
    return cast;
  }

  // Generate MIR that computes a boolean value indicating whether or not it
  // is possible to cast `ref` to `destType`.
  [[nodiscard]] MDefinition* refTest(MDefinition* ref, RefType destType) {
    MInstruction* isSubTypeOf = nullptr;
    if (destType.isTypeRef()) {
      uint32_t typeIndex = codeMeta().types->indexOf(*destType.typeDef());
      MDefinition* superSTV = loadSuperTypeVector(typeIndex);
      if (!superSTV) {
        return nullptr;
      }
      isSubTypeOf = MWasmRefTestConcrete::New(alloc(), ref, superSTV, destType);
    } else {
      isSubTypeOf = MWasmRefTestAbstract::New(alloc(), ref, destType);
    }
    MOZ_ASSERT(isSubTypeOf);

    curBlock_->add(isSubTypeOf);
    return isSubTypeOf;
  }

  // Generates MIR for br_on_cast and br_on_cast_fail.
  [[nodiscard]] bool brOnCastCommon(bool onSuccess, uint32_t labelRelativeDepth,
                                    RefType sourceType, RefType destType,
                                    const ResultType& labelType,
                                    const DefVector& values,
                                    BranchHint branchHint) {
    if (inDeadCode()) {
      return true;
    }

    MBasicBlock* fallthroughBlock = nullptr;
    if (!newBlock(curBlock_, &fallthroughBlock)) {
      return false;
    }

    // `values` are the values in the top block-value on the stack.  Since the
    // argument to `br_on_cast{_fail}` is at the top of the stack, it is the
    // last element in `values`.
    //
    // For both br_on_cast and br_on_cast_fail, the OpIter validation routines
    // ensure that `values` is non-empty (by rejecting the case
    // `labelType->length() < 1`) and that the last value in `values` is
    // reftyped.
    MOZ_RELEASE_ASSERT(values.length() > 0);
    MDefinition* ref = values.back();
    MOZ_ASSERT(ref->type() == MIRType::WasmAnyRef);

    MDefinition* success = refTest(ref, destType);
    if (!success) {
      return false;
    }

    MTest* test;
    if (onSuccess) {
      test = MTest::New(alloc(), success, nullptr, fallthroughBlock);
      if (!test || !addControlFlowPatch(test, labelRelativeDepth,
                                        MTest::TrueBranchIndex, branchHint)) {
        return false;
      }
    } else {
      test = MTest::New(alloc(), success, fallthroughBlock, nullptr);
      if (!test || !addControlFlowPatch(test, labelRelativeDepth,
                                        MTest::FalseBranchIndex, branchHint)) {
        return false;
      }
    }

    if (!pushDefs(values)) {
      return false;
    }

    curBlock_->end(test);
    curBlock_ = fallthroughBlock;
    return true;
  }

  [[nodiscard]] bool brOnNonStruct(const DefVector& values) {
    if (inDeadCode()) {
      return true;
    }

    MBasicBlock* fallthroughBlock = nullptr;
    if (!newBlock(curBlock_, &fallthroughBlock)) {
      return false;
    }

    MOZ_ASSERT(values.length() > 0);
    MOZ_ASSERT(values.back()->type() == MIRType::WasmAnyRef);

    MGoto* jump = MGoto::New(alloc(), fallthroughBlock);
    if (!jump) {
      return false;
    }
    if (!pushDefs(values)) {
      return false;
    }

    curBlock_->end(jump);
    curBlock_ = fallthroughBlock;
    return true;
  }

  [[nodiscard]] MDefinition* convertAnyExtern(MDefinition* ref,
                                              wasm::RefType::Kind kind) {
    auto* converted = MWasmRefConvertAnyExtern::New(alloc(), ref, kind);
    if (!converted) {
      return nullptr;
    }
    curBlock_->add(converted);
    return converted;
  }

  /************************************************************ DECODING ***/

  uint32_t readBytecodeOffset() { return currentOpOffset_; }

  // Report a rejection this side of the boundary noticed, at the current
  // operator's offset.
  bool fail(const char* msg) { return decoder_.fail(currentOpOffset_, msg); }

  CallRefHint readCallRefHint() {
    // We don't track anything if we're not using lazy tiering
    if (compilerEnv().mode() != CompileMode::LazyTiering) {
      return CallRefHint();
    }

    CallRefMetricsRange rangeInModule =
        codeTailMeta()->getFuncDefCallRefs(funcIndex());
    uint32_t localIndex = numCallRefs_++;
    MOZ_RELEASE_ASSERT(localIndex < rangeInModule.length);
    uint32_t moduleIndex = rangeInModule.begin + localIndex;
    return codeTailMeta()->getCallRefHint(moduleIndex);
  }

  /*************************************************************************/
 private:
  [[nodiscard]] bool newBlock(MBasicBlock* pred, MBasicBlock** block,
                              MBasicBlock::Kind kind = MBasicBlock::NORMAL) {
    *block = MBasicBlock::New(mirGraph(), info(), pred, kind);
    if (!*block) {
      return false;
    }
    mirGraph().addBlock(*block);
    (*block)->setLoopDepth(rootCompiler_.loopDepth());
    return true;
  }

  [[nodiscard]] bool goToNewBlock(MBasicBlock* pred, MBasicBlock** block) {
    if (!newBlock(pred, block)) {
      return false;
    }
    pred->end(MGoto::New(alloc(), *block));
    return true;
  }

  [[nodiscard]] bool goToExistingBlock(MBasicBlock* prev, MBasicBlock* next) {
    MOZ_ASSERT(prev);
    MOZ_ASSERT(next);
    prev->end(MGoto::New(alloc(), next));
    return next->addPredecessor(alloc(), prev);
  }

  [[nodiscard]] bool bindBranches(uint32_t absolute, DefVector* defs) {
    if (absolute >= pendingBlocks_.length() ||
        pendingBlocks_[absolute].patches.empty()) {
      return inDeadCode() || popPushedDefs(defs);
    }

    ControlFlowPatchVector& patches = pendingBlocks_[absolute].patches;
    MControlInstruction* ins = patches[0].ins;
    MBasicBlock* pred = ins->block();

    MBasicBlock* join = nullptr;
    if (!newBlock(pred, &join)) {
      return false;
    }

    // Use branch hinting information if any.
    if (pendingBlocks_[absolute].hint != BranchHint::Invalid) {
      BranchHint hint = pendingBlocks_[absolute].hint;
      if (hint == BranchHint::Likely) {
        join->setFrequency(Frequency::Likely);
      } else if (hint == BranchHint::Unlikely) {
        join->setFrequency(Frequency::Unlikely);
      }
    }

    pred->mark();
    ins->replaceSuccessor(patches[0].index, join);

    for (size_t i = 1; i < patches.length(); i++) {
      ins = patches[i].ins;

      pred = ins->block();
      if (!pred->isMarked()) {
        if (!join->addPredecessor(alloc(), pred)) {
          return false;
        }
        pred->mark();
      }

      ins->replaceSuccessor(patches[i].index, join);
    }

    MOZ_ASSERT_IF(curBlock_, !curBlock_->isMarked());
    for (uint32_t i = 0; i < join->numPredecessors(); i++) {
      join->getPredecessor(i)->unmark();
    }

    if (curBlock_ && !goToExistingBlock(curBlock_, join)) {
      return false;
    }

    curBlock_ = join;

    if (!popPushedDefs(defs)) {
      return false;
    }

    patches.clear();
    return true;
  }

  bool emitI32Const(int32_t i32);
  bool emitI64Const(int64_t i64);
  bool emitF32Const(float f32);
  bool emitF64Const(double f64);
  bool emitBlock(BlockType type);
  bool emitLoop(BlockType type);
  bool emitIf(BlockType type);
  bool emitElse();
  bool emitEnd();
  bool emitBr(uint32_t relativeDepth);
  bool emitBrIf(uint32_t relativeDepth);
  bool emitBrTable(const Uint32Vector& depths, uint32_t defaultDepth);
  bool emitReturn();
  bool emitUnreachable();
  bool emitInlineCall(const FuncType& funcType, uint32_t funcIndex,
                      InliningHeuristics::CallKind callKind,
                      const DefVector& args, DefVector* results);
  bool emitCall(uint32_t funcIndex);
  bool emitCallIndirect(uint32_t funcTypeIndex);
#ifdef ENABLE_WASM_JSPI
  bool emitContNew();
  bool emitContBind();
  [[nodiscard]] bool makeSwitchStackResultArea(const ValTypeVector& types,
                                               MWasmStackResultArea** out);
  bool emitStoreSuspendParams(MDefinition* paramsArea,
                              const ValTypeVector& suspendTagParams,
                              const DefVector& suspendParams,
                              MDefinition* suspendedCont);
  bool emitSuspend();
  bool emitResume();
  bool emitResumeThrow();
  bool emitResumeThrowRef();
  bool emitSwitch();
  bool emitGuardSuspending();
#endif
  bool emitGetLocal(uint32_t id);
  bool emitSetLocal(uint32_t id);
  bool emitTeeLocal(uint32_t id);
  bool emitGetGlobal(uint32_t id);
  bool emitSetGlobal(uint32_t id);
  bool emitTeeGlobal();
  template <typename MIRClass>
  bool emitConversion(ValType operandType, ValType resultType);
  template <typename MIRClass>
  bool emitUnaryWithType(ValType operandType, MIRType mirType);
  bool emitTruncate(ValType operandType, ValType resultType, bool isUnsigned,
                    bool isSaturating);
  bool emitExtendI32(bool isUnsigned);
  bool emitWrapI32();
  bool emitConvertI64ToFloatingPoint(ValType resultType, MIRType mirType,
                                     bool isUnsigned);
  bool emitReinterpret(ValType resultType, ValType operandType,
                       MIRType mirType);
  bool emitAdd(ValType type, MIRType mirType);
  bool emitSub(ValType type, MIRType mirType);
  bool emitRotate(ValType type, bool isLeftRotation);
  bool emitBitwiseAndOrXor(ValType operandType, MIRType mirType,
                           MWasmBinaryBitwise::SubOpcode subOpc);
  template <typename MIRClass>
  bool emitShift(ValType operandType, MIRType mirType);
  bool emitUrsh(ValType operandType, MIRType mirType);
  bool emitMul(ValType operandType, MIRType mirType);
  bool emitDiv(ValType operandType, MIRType mirType, bool isUnsigned);
  bool emitRem(ValType operandType, MIRType mirType, bool isUnsigned);
  bool emitMinMax(ValType operandType, MIRType mirType, bool isMax);
  bool emitCopySign(ValType operandType);
  bool emitComparison(ValType operandType, JSOp compareOp,
                      MCompare::CompareType compareType);
  bool emitSelect(bool typed);
  bool emitLoad(ValType type, Scalar::Type viewType, uint32_t alignLog2,
                uint32_t offset);
  bool emitStore(ValType resultType, Scalar::Type viewType, uint32_t alignLog2,
                 uint32_t offset);
  bool tryInlineUnaryBuiltin(SymbolicAddress callee, MDefinition* input);
  bool emitUnaryMathBuiltinCall(const SymbolicAddressSignature& callee);
  bool emitMemoryGrow();
  bool emitMemorySize();
  bool emitMemCopyCall(uint32_t dstMemIndex, uint32_t srcMemIndex,
                       MDefinition* dst, MDefinition* src, MDefinition* len);
  bool emitMemCopyInline(uint32_t memoryIndex, MDefinition* dst,
                         MDefinition* src, uint32_t length);
  bool emitMemFillCall(uint32_t memoryIndex, MDefinition* start,
                       MDefinition* val, MDefinition* len);
  bool emitMemFillInline(uint32_t memoryIndex, MDefinition* start,
                         MDefinition* val, uint32_t length);
  bool emitConstSimd128();
  bool emitBinarySimd128(bool commutative, SimdOp op);
  bool emitTernarySimd128(wasm::SimdOp op);
  bool emitShiftSimd128(SimdOp op);
  bool emitSplatSimd128(ValType inType, SimdOp op);
  bool emitUnarySimd128(SimdOp op);
  bool emitReduceSimd128(SimdOp op);
  bool emitExtractLaneSimd128(ValType outType, uint32_t laneLimit, SimdOp op);
  bool emitReplaceLaneSimd128(ValType laneType, uint32_t laneLimit, SimdOp op);
  bool emitShuffleSimd128();
  bool emitLoadSplatSimd128(Scalar::Type viewType, wasm::SimdOp splatOp);
  bool emitLoadExtendSimd128(wasm::SimdOp op);
  bool emitLoadZeroSimd128(Scalar::Type viewType, size_t numBytes);
  bool emitLoadLaneSimd128(uint32_t laneSize);
  bool emitStoreLaneSimd128(uint32_t laneSize);
  bool emitSpeculativeInlineCallRef(uint32_t bytecodeOffset,
                                    const FuncType& funcType,
                                    CallRefHint expectedFuncIndices,
                                    MDefinition* actualCalleeFunc,
                                    const DefVector& args, DefVector* results);

 public:
  bool beginPushedBody();
  bool beginPushedOp(OpBytes op, size_t opOffset);
};

template <>
MDefinition* FunctionCompiler::unary<MToFloat32>(MDefinition* op) {
  if (inDeadCode()) {
    return nullptr;
  }
  auto* ins = MToFloat32::New(alloc(), op, mustPreserveNaN(op->type()));
  curBlock_->add(ins);
  return ins;
}

template <>
MDefinition* FunctionCompiler::unary<MNot>(MDefinition* op) {
  if (inDeadCode()) {
    return nullptr;
  }
  auto* ins = MNot::NewInt32(alloc(), op);
  curBlock_->add(ins);
  return ins;
}

template <>
MDefinition* FunctionCompiler::unary<MAbs>(MDefinition* op, MIRType type) {
  if (inDeadCode()) {
    return nullptr;
  }
  auto* ins = MAbs::NewWasm(alloc(), op, type);
  curBlock_->add(ins);
  return ins;
}

bool FunctionCompiler::emitI32Const(int32_t i32) {
  return push(constantI32(i32));
}

bool FunctionCompiler::emitI64Const(int64_t i64) {
  return push(constantI64(i64));
}

bool FunctionCompiler::emitF32Const(float f32) {
  return push(constantF32(f32));
}

bool FunctionCompiler::emitF64Const(double f64) {
  return push(constantF64(f64));
}

bool FunctionCompiler::emitBlock(BlockType type) {
  return pushControl(LabelKind::Block, type) && startBlock();
}

bool FunctionCompiler::emitLoop(BlockType type) {
  if (!pushControl(LabelKind::Loop, type)) {
    return false;
  }

  MBasicBlock* loopHeader;
  if (!startLoop(&loopHeader, type.params().length())) {
    return false;
  }

  addInterruptCheck();

  controlItem().block = loopHeader;
  return true;
}

bool FunctionCompiler::emitIf(BlockType type) {
  MDefinition* condition = pop();

  if (!pushControl(LabelKind::Then, type)) {
    return false;
  }

  MBasicBlock* elseBlock;
  if (!branchAndStartThen(condition, &elseBlock)) {
    return false;
  }

  controlItem().block = elseBlock;
  return true;
}

bool FunctionCompiler::emitElse() {
  ControlFrame& top = controlStack_.back();
  ResultType resultType = top.type.results();

  DefVector thenValues;
  if (!peekN(resultType.length(), &thenValues)) {
    return false;
  }
  valueStack_.shrinkTo(top.valueStackBase);
  top.kind = LabelKind::Else;

  if (!pushDefs(thenValues)) {
    return false;
  }

  Control& control = top.item;
  return switchToElse(control.block, &control.block);
}

bool FunctionCompiler::emitEnd() {
  LabelKind kind = controlStack_.back().kind;
  ResultType type = controlStack_.back().type.results();

  DefVector preJoinDefs;
  if (!peekN(type.length(), &preJoinDefs)) {
    return false;
  }

  Control& control = controlItem();
  MBasicBlock* block = control.block;

  if (!pushDefs(preJoinDefs)) {
    return false;
  }

  // Every label case is responsible to pop the control item at the appropriate
  // time for the label case
  DefVector postJoinDefs;
  switch (kind) {
    case LabelKind::Body: {
      MOZ_ASSERT(!control.tryControl);
      if (!emitBodyRethrowPad(control)) {
        return false;
      }
      if (!finishBlock(&postJoinDefs)) {
        return false;
      }
      if (!returnValues(std::move(postJoinDefs))) {
        return false;
      }
      controlStack_.popBack();
      MOZ_ASSERT(controlStack_.empty());
      // Where the body ended is the validator's to say, so there is no decoder
      // position to check here.
      return true;
    }
    case LabelKind::Block:
      MOZ_ASSERT(!control.tryControl);
      if (!finishBlock(&postJoinDefs)) {
        return false;
      }
      controlStack_.popBack();
      break;
    case LabelKind::Loop:
      MOZ_ASSERT(!control.tryControl);
      if (!closeLoop(block, &postJoinDefs)) {
        return false;
      }
      controlStack_.popBack();
      break;
    case LabelKind::Then: {
      MOZ_ASSERT(!control.tryControl);
      // If we didn't see an Else, create a trivial else block so that we create
      // a diamond anyway, to preserve Ion invariants.
      if (!switchToElse(block, &block)) {
        return false;
      }

      // wasm 1.0 blocks take no parameters, so an elided else has no results
      // to pass through.
      if (!pushDefs(DefVector())) {
        return false;
      }

      if (!joinIfElse(block, &postJoinDefs)) {
        return false;
      }
      controlStack_.popBack();
      break;
    }
    case LabelKind::Else:
      MOZ_ASSERT(!control.tryControl);
      if (!joinIfElse(block, &postJoinDefs)) {
        return false;
      }
      controlStack_.popBack();
      break;
    case LabelKind::Try:
    case LabelKind::Catch:
    case LabelKind::CatchAll:
    case LabelKind::TryTable:
      // wasm 1.0 has no exceptions: no hook can ever push a Try/TryTable
      // control frame, so this label kind cannot occur.
      MOZ_CRASH("unreachable: wasm 1.0 has no exceptions");
  }

  MOZ_ASSERT_IF(!inDeadCode(), postJoinDefs.length() == type.length());
  pokeN(postJoinDefs.length(), postJoinDefs);

  return true;
}

bool FunctionCompiler::emitBr(uint32_t relativeDepth) {
  ResultType type = branchTargetType(relativeDepth);
  DefVector values;
  if (!peekN(type.length(), &values)) {
    return false;
  }
  afterUnconditionalBranch();

  return br(relativeDepth, values);
}

bool FunctionCompiler::emitBrIf(uint32_t relativeDepth) {
  MDefinition* condition = pop();

  ResultType type = branchTargetType(relativeDepth);
  DefVector values;
  if (!peekN(type.length(), &values)) {
    return false;
  }

  return brIf(relativeDepth, values, condition, BranchHint::Invalid);
}

bool FunctionCompiler::emitBrTable(const Uint32Vector& depths,
                                   uint32_t defaultDepth) {
  MDefinition* index = pop();

  // `branchValueType` is the type of the default branch target. In the
  // presence of subtyping, it could be that the different branch targets
  // have different types. We rely on the assumption that the value
  // representations of all branch target types are the same in Ion, as
  // baseline does.
  ResultType branchValueType = branchTargetType(defaultDepth);
  DefVector branchValues;
  if (!peekN(branchValueType.length(), &branchValues)) {
    return false;
  }
  afterUnconditionalBranch();

  // If all the targets are the same, or there are no targets, we can just
  // use a goto. This is not just an optimization: MaybeFoldConditionBlock
  // assumes that tables have more than one successor.
  bool allSameDepth = true;
  for (uint32_t depth : depths) {
    if (depth != defaultDepth) {
      allSameDepth = false;
      break;
    }
  }

  if (allSameDepth) {
    return br(defaultDepth, branchValues);
  }

  return brTable(index, defaultDepth, depths, branchValues);
}

bool FunctionCompiler::emitReturn() {
  ResultType type = controlStack_[0].type.results();
  DefVector values;
  if (!popN(type.length(), &values)) {
    return false;
  }
  afterUnconditionalBranch();

  return returnValues(std::move(values));
}

bool FunctionCompiler::emitUnreachable() {
  afterUnconditionalBranch();
  unreachableTrap();
  return true;
}

bool FunctionCompiler::emitInlineCall(const FuncType& funcType,
                                      uint32_t funcIndex,
                                      InliningHeuristics::CallKind callKind,
                                      const DefVector& args,
                                      DefVector* results) {
  // The POC does not inline. Ion inlines a callee by compiling its body, and a
  // body is compiled by driving itasca over the module's bytes against the
  // environment it validated; an inlined callee is reached again later, when
  // neither is at hand. InliningHeuristics declines every candidate, so this
  // is unreachable.
  MOZ_CRASH("wasm inlining is disabled on the itasca branch");
}

bool FunctionCompiler::emitCall(uint32_t funcIndex) {
  uint32_t bytecodeOffset = readBytecodeOffset();
  const FuncType& funcType = codeMeta().getFuncType(funcIndex);

  DefVector args;
  if (!popN(funcType.args().length(), &args)) {
    return false;
  }

  if (inDeadCode()) {
    for (size_t i = 0; i < funcType.results().length(); i++) {
      if (!push(nullptr)) {
        return false;
      }
    }
    return true;
  }

  DefVector results;
  if (codeMeta().funcIsImport(funcIndex)) {
    BuiltinModuleFuncId knownFuncImport = codeMeta().knownFuncImport(funcIndex);
    if (knownFuncImport != BuiltinModuleFuncId::None) {
      const BuiltinModuleFunc& builtinModuleFunc =
          BuiltinModuleFuncs::getFromId(knownFuncImport);
      return callBuiltinModuleFunc(builtinModuleFunc, args);
    }

    uint32_t instanceDataOffset =
        codeMeta().offsetOfFuncImportInstanceData(funcIndex);
    if (!callImport(instanceDataOffset, bytecodeOffset, funcType, args,
                    &results)) {
      return false;
    }
  } else {
    const auto callKind = InliningHeuristics::CallKind::Direct;
    // Make up a single-entry CallRefHint and enquire about its inlineability.
    CallRefHint hints;
    hints.append(funcIndex);
    hints = auditInlineableCallees(callKind, hints);
    if (!hints.empty()) {
      // Inlining of `funcIndex` was approved.
      if (!emitInlineCall(funcType, funcIndex, callKind, args, &results)) {
        return false;
      }
    } else {
      if (!callDirect(funcType, funcIndex, bytecodeOffset, args, &results)) {
        return false;
      }
    }
  }

  return pushN(results);
}

bool FunctionCompiler::emitCallIndirect(uint32_t funcTypeIndex) {
  uint32_t bytecodeOffset = readBytecodeOffset();

  // wasm 1.0 has one table, named by a reserved zero byte the validator
  // already checked.
  const uint32_t tableIndex = 0;
  const FuncType& funcType = codeMeta().types->type(funcTypeIndex).funcType();

  MDefinition* callee = pop();
  DefVector args;
  if (!popN(funcType.args().length(), &args)) {
    return false;
  }

  if (inDeadCode()) {
    for (size_t i = 0; i < funcType.results().length(); i++) {
      if (!push(nullptr)) {
        return false;
      }
    }
    return true;
  }

  DefVector results;
  if (!callIndirect(funcTypeIndex, tableIndex, callee, bytecodeOffset, args,
                    &results)) {
    return false;
  }

  return pushN(results);
}

bool FunctionCompiler::emitGetLocal(uint32_t id) {
  return push(getLocalDef(id));
}

bool FunctionCompiler::emitSetLocal(uint32_t id) {
  MDefinition* value = pop();
  assign(id, value);
  return true;
}

bool FunctionCompiler::emitTeeLocal(uint32_t id) {
  // tee leaves its value in place: peek it rather than popping and pushing.
  MDefinition* value = pop();
  if (!push(value)) {
    return false;
  }
  assign(id, value);
  return true;
}

bool FunctionCompiler::emitGetGlobal(uint32_t id) {
  const GlobalDesc& global = codeMeta().globals[id];
  if (!global.isConstant()) {
    return push(loadGlobalVar(global));
  }

  LitVal value = global.constantValue();

  MDefinition* result;
  switch (value.type().kind()) {
    case ValType::I32:
      result = constantI32(int32_t(value.i32()));
      break;
    case ValType::I64:
      result = constantI64(int64_t(value.i64()));
      break;
    case ValType::F32:
      result = constantF32(value.f32());
      break;
    case ValType::F64:
      result = constantF64(value.f64());
      break;
    case ValType::V128:
#ifdef ENABLE_WASM_SIMD
      result = constantV128(value.v128());
      break;
#else
      return fail("Ion has no SIMD support yet");
#endif
    case ValType::Ref:
      MOZ_ASSERT(value.ref().isNull());
      result = constantNullRef(MaybeRefType(value.type().refType()));
      break;
    default:
      MOZ_CRASH("unexpected type in EmitGetGlobal");
  }

  return push(result);
}

bool FunctionCompiler::emitSetGlobal(uint32_t id) {
  uint32_t bytecodeOffset = readBytecodeOffset();
  MDefinition* value = pop();

  const GlobalDesc& global = codeMeta().globals[id];
  MOZ_ASSERT(global.isMutable());
  return storeGlobalVar(bytecodeOffset, global, value);
}

template <typename MIRClass>
bool FunctionCompiler::emitConversion(ValType operandType, ValType resultType) {
  MDefinition* input = pop();

  return push(unary<MIRClass>(input));
}

template <typename MIRClass>
bool FunctionCompiler::emitUnaryWithType(ValType operandType, MIRType mirType) {
  MDefinition* input = pop();

  return push(unary<MIRClass>(input, mirType));
}

bool FunctionCompiler::emitTruncate(ValType operandType, ValType resultType,
                                    bool isUnsigned, bool isSaturating) {
  MDefinition* input = pop();

  TruncFlags flags = 0;
  if (isUnsigned) {
    flags |= TRUNC_UNSIGNED;
  }
  if (isSaturating) {
    flags |= TRUNC_SATURATING;
  }
  if (resultType == ValType::I32) {
    return push(truncate<MWasmTruncateToInt32>(input, flags));
  }
  MOZ_ASSERT(resultType == ValType::I64);
#if defined(JS_CODEGEN_ARM)
  return push(truncateWithInstance(input, flags));
#else
  return push(truncate<MWasmTruncateToInt64>(input, flags));
#endif
}

bool FunctionCompiler::emitExtendI32(bool isUnsigned) {
  MDefinition* input = pop();

  return push(extendI32(input, isUnsigned));
}

bool FunctionCompiler::emitWrapI32() {
  MDefinition* input = pop();

  return push(wrapI32(input));
}

bool FunctionCompiler::emitConvertI64ToFloatingPoint(ValType resultType,
                                                     MIRType mirType,
                                                     bool isUnsigned) {
  MDefinition* input = pop();

  return push(convertI64ToFloatingPoint(input, mirType, isUnsigned));
}

bool FunctionCompiler::emitReinterpret(ValType resultType, ValType operandType,
                                       MIRType mirType) {
  MDefinition* input = pop();

  return push(unary<MReinterpretCast>(input, mirType));
}

bool FunctionCompiler::emitAdd(ValType type, MIRType mirType) {
  MDefinition* lhs;
  MDefinition* rhs;
  popBinary(&lhs, &rhs);

  return push(add(lhs, rhs, mirType));
}

bool FunctionCompiler::emitSub(ValType type, MIRType mirType) {
  MDefinition* lhs;
  MDefinition* rhs;
  popBinary(&lhs, &rhs);

  return push(sub(lhs, rhs, mirType));
}

bool FunctionCompiler::emitRotate(ValType type, bool isLeftRotation) {
  MDefinition* lhs;
  MDefinition* rhs;
  popBinary(&lhs, &rhs);

  MDefinition* result = rotate(lhs, rhs, type.toMIRType(), isLeftRotation);
  return push(result);
}

bool FunctionCompiler::emitBitwiseAndOrXor(
    ValType operandType, MIRType mirType,
    MWasmBinaryBitwise::SubOpcode subOpc) {
  MDefinition* lhs;
  MDefinition* rhs;
  popBinary(&lhs, &rhs);

  return push(binary<MWasmBinaryBitwise>(lhs, rhs, mirType, subOpc));
}

template <typename MIRClass>
bool FunctionCompiler::emitShift(ValType operandType, MIRType mirType) {
  MDefinition* lhs;
  MDefinition* rhs;
  popBinary(&lhs, &rhs);

  return push(binary<MIRClass>(lhs, rhs, mirType));
}

bool FunctionCompiler::emitUrsh(ValType operandType, MIRType mirType) {
  MDefinition* lhs;
  MDefinition* rhs;
  popBinary(&lhs, &rhs);

  return push(ursh(lhs, rhs, mirType));
}

bool FunctionCompiler::emitMul(ValType operandType, MIRType mirType) {
  MDefinition* lhs;
  MDefinition* rhs;
  popBinary(&lhs, &rhs);

  return push(mul(lhs, rhs, mirType,
                  mirType == MIRType::Int32 ? MMul::Integer : MMul::Normal));
}

bool FunctionCompiler::emitDiv(ValType operandType, MIRType mirType,
                               bool isUnsigned) {
  MDefinition* lhs;
  MDefinition* rhs;
  popBinary(&lhs, &rhs);

  return push(div(lhs, rhs, mirType, isUnsigned));
}

bool FunctionCompiler::emitRem(ValType operandType, MIRType mirType,
                               bool isUnsigned) {
  MDefinition* lhs;
  MDefinition* rhs;
  popBinary(&lhs, &rhs);

  return push(mod(lhs, rhs, mirType, isUnsigned));
}

bool FunctionCompiler::emitMinMax(ValType operandType, MIRType mirType,
                                  bool isMax) {
  MDefinition* lhs;
  MDefinition* rhs;
  popBinary(&lhs, &rhs);

  return push(minMax(lhs, rhs, mirType, isMax));
}

bool FunctionCompiler::emitCopySign(ValType operandType) {
  MDefinition* lhs;
  MDefinition* rhs;
  popBinary(&lhs, &rhs);

  return push(binary<MCopySign>(lhs, rhs, operandType.toMIRType()));
}

bool FunctionCompiler::emitComparison(ValType operandType, JSOp compareOp,
                                      MCompare::CompareType compareType) {
  MDefinition* lhs;
  MDefinition* rhs;
  popBinary(&lhs, &rhs);

  return push(compare(lhs, rhs, compareOp, compareType));
}

bool FunctionCompiler::emitSelect(bool typed) {
  // select() reads its type off trueValue itself, so the operand type wasm
  // 1.0's untyped select carries is unused here.
  MDefinition* condition = pop();
  MDefinition* falseValue = pop();
  MDefinition* trueValue = pop();

  return push(select(trueValue, falseValue, condition));
}

bool FunctionCompiler::emitLoad(ValType type, Scalar::Type viewType,
                                uint32_t alignLog2, uint32_t offset) {
  // wasm 1.0 has one memory and no memory index in the memarg.
  MDefinition* base = pop();
  MemoryAccessDesc access(/*memoryIndex=*/0, viewType, uint32_t(1) << alignLog2,
                          offset, trapSiteDesc(),
                          hugeMemoryEnabled(/*memoryIndex=*/0));
  auto* ins = load(base, &access, type);
  if (!inDeadCode() && !ins) {
    return false;
  }

  return push(ins);
}

bool FunctionCompiler::emitStore(ValType resultType, Scalar::Type viewType,
                                 uint32_t alignLog2, uint32_t offset) {
  MDefinition* value = pop();
  MDefinition* base = pop();

  // wasm 1.0 has one memory and no memory index in the memarg.
  MemoryAccessDesc access(/*memoryIndex=*/0, viewType, uint32_t(1) << alignLog2,
                          offset, trapSiteDesc(),
                          hugeMemoryEnabled(/*memoryIndex=*/0));

  return store(base, &access, value);
}

bool FunctionCompiler::tryInlineUnaryBuiltin(SymbolicAddress callee,
                                             MDefinition* input) {
  if (!input) {
    return false;
  }

  MOZ_ASSERT(IsFloatingPointType(input->type()));

  RoundingMode mode;
  if (!IsRoundingFunction(callee, &mode)) {
    return false;
  }

  if (!MNearbyInt::HasAssemblerSupport(mode)) {
    return false;
  }

  return push(nearbyInt(input, mode));
}

bool FunctionCompiler::emitUnaryMathBuiltinCall(
    const SymbolicAddressSignature& callee) {
  MOZ_ASSERT(callee.numArgs == 1);

  uint32_t bytecodeOffset = readBytecodeOffset();

  MDefinition* input = pop();

  if (tryInlineUnaryBuiltin(callee.identity, input)) {
    return true;
  }

  MDefinition* def;
  if (!builtinCall1(callee, bytecodeOffset, input, &def)) {
    return false;
  }

  return push(def);
}

bool FunctionCompiler::emitMemoryGrow() {
  uint32_t bytecodeOffset = readBytecodeOffset();

  // wasm 1.0 has one memory, named by a reserved zero byte the validator
  // already checked.
  const uint32_t memoryIndex = 0;
  MDefinition* delta = pop();

  if (inDeadCode()) {
    return push(nullptr);
  }

  MDefinition* memoryIndexValue = constantI32(int32_t(memoryIndex));
  if (!memoryIndexValue) {
    return false;
  }

  const SymbolicAddressSignature& callee =
      isMem32(memoryIndex) ? SASigMemoryGrowM32 : SASigMemoryGrowM64;

  MDefinition* ret;
  if (!emitInstanceCall2(bytecodeOffset, callee, delta, memoryIndexValue,
                         &ret)) {
    return false;
  }

  return push(ret);
}

bool FunctionCompiler::emitMemorySize() {
  uint32_t bytecodeOffset = readBytecodeOffset();

  const uint32_t memoryIndex = 0;

  if (inDeadCode()) {
    return push(nullptr);
  }

  MDefinition* memoryIndexValue = constantI32(int32_t(memoryIndex));
  if (!memoryIndexValue) {
    return false;
  }

  const SymbolicAddressSignature& callee =
      isMem32(memoryIndex) ? SASigMemorySizeM32 : SASigMemorySizeM64;

  MDefinition* ret;
  if (!emitInstanceCall1(bytecodeOffset, callee, memoryIndexValue, &ret)) {
    return false;
  }

  return push(ret);
}

bool FunctionCompiler::emitMemCopyCall(uint32_t dstMemIndex,
                                       uint32_t srcMemIndex, MDefinition* dst,
                                       MDefinition* src, MDefinition* len) {
  uint32_t bytecodeOffset = readBytecodeOffset();

  if (dstMemIndex == srcMemIndex) {
    const SymbolicAddressSignature& callee =
        (codeMeta().usesSharedMemory(dstMemIndex)
             ? (isMem32(dstMemIndex) ? SASigMemCopySharedM32
                                     : SASigMemCopySharedM64)
             : (isMem32(dstMemIndex) ? SASigMemCopyM32 : SASigMemCopyM64));
    MDefinition* base = memoryBase(dstMemIndex);
    if (!base) {
      return false;
    }
    return emitInstanceCall4(bytecodeOffset, callee, dst, src, len, base);
  }

  AddressType dstIndexType = codeMeta().memories[dstMemIndex].addressType();
  AddressType srcIndexType = codeMeta().memories[srcMemIndex].addressType();

  if (dstIndexType == AddressType::I32) {
    dst = extendI32(dst, /*isUnsigned=*/true);
    if (!dst) {
      return false;
    }
  }
  if (srcIndexType == AddressType::I32) {
    src = extendI32(src, /*isUnsigned=*/true);
    if (!src) {
      return false;
    }
  }
  if (dstIndexType == AddressType::I32 || srcIndexType == AddressType::I32) {
    len = extendI32(len, /*isUnsigned=*/true);
    if (!len) {
      return false;
    }
  }

  MDefinition* dstMemIndexValue = constantI32(int32_t(dstMemIndex));
  if (!dstMemIndexValue) {
    return false;
  }

  MDefinition* srcMemIndexValue = constantI32(int32_t(srcMemIndex));
  if (!srcMemIndexValue) {
    return false;
  }

  return emitInstanceCall5(bytecodeOffset, SASigMemCopyAny, dst, src, len,
                           dstMemIndexValue, srcMemIndexValue);
}

bool FunctionCompiler::emitMemCopyInline(uint32_t memoryIndex, MDefinition* dst,
                                         MDefinition* src, uint32_t length) {
  MOZ_ASSERT(length != 0 && length <= MaxInlineMemoryCopyLength);

  // Compute the number of copies of each width we will need to do
  size_t remainder = length;
#ifdef ENABLE_WASM_SIMD
  size_t numCopies16 = 0;
  if (MacroAssembler::SupportsFastUnalignedFPAccesses()) {
    numCopies16 = remainder / sizeof(V128);
    remainder %= sizeof(V128);
  }
#endif
#ifdef JS_64BIT
  size_t numCopies8 = remainder / sizeof(uint64_t);
  remainder %= sizeof(uint64_t);
#endif
  size_t numCopies4 = remainder / sizeof(uint32_t);
  remainder %= sizeof(uint32_t);
  size_t numCopies2 = remainder / sizeof(uint16_t);
  remainder %= sizeof(uint16_t);
  size_t numCopies1 = remainder;

  // Load all source bytes from low to high using the widest transfer width we
  // can for the system. We will trap without writing anything if any source
  // byte is out-of-bounds.
  size_t offset = 0;
  DefVector loadedValues;

#ifdef ENABLE_WASM_SIMD
  for (uint32_t i = 0; i < numCopies16; i++) {
    MemoryAccessDesc access(memoryIndex, Scalar::Simd128, 1, offset,
                            trapSiteDesc(), hugeMemoryEnabled(memoryIndex));
    auto* loadValue = load(src, &access, ValType::V128);
    if (!loadValue || !loadedValues.append(loadValue)) {
      return false;
    }

    offset += sizeof(V128);
  }
#endif

#ifdef JS_64BIT
  for (uint32_t i = 0; i < numCopies8; i++) {
    MemoryAccessDesc access(memoryIndex, Scalar::Int64, 1, offset,
                            trapSiteDesc(), hugeMemoryEnabled(memoryIndex));
    auto* loadValue = load(src, &access, ValType::I64);
    if (!loadValue || !loadedValues.append(loadValue)) {
      return false;
    }

    offset += sizeof(uint64_t);
  }
#endif

  for (uint32_t i = 0; i < numCopies4; i++) {
    MemoryAccessDesc access(memoryIndex, Scalar::Uint32, 1, offset,
                            trapSiteDesc(), hugeMemoryEnabled(memoryIndex));
    auto* loadValue = load(src, &access, ValType::I32);
    if (!loadValue || !loadedValues.append(loadValue)) {
      return false;
    }

    offset += sizeof(uint32_t);
  }

  if (numCopies2) {
    MemoryAccessDesc access(memoryIndex, Scalar::Uint16, 1, offset,
                            trapSiteDesc(), hugeMemoryEnabled(memoryIndex));
    auto* loadValue = load(src, &access, ValType::I32);
    if (!loadValue || !loadedValues.append(loadValue)) {
      return false;
    }

    offset += sizeof(uint16_t);
  }

  if (numCopies1) {
    MemoryAccessDesc access(memoryIndex, Scalar::Uint8, 1, offset,
                            trapSiteDesc(), hugeMemoryEnabled(memoryIndex));
    auto* loadValue = load(src, &access, ValType::I32);
    if (!loadValue || !loadedValues.append(loadValue)) {
      return false;
    }
  }

  // Store all source bytes to the destination from high to low. We will trap
  // without writing anything on the first store if any dest byte is
  // out-of-bounds.
  offset = length;

  if (numCopies1) {
    offset -= sizeof(uint8_t);

    MemoryAccessDesc access(memoryIndex, Scalar::Uint8, 1, offset,
                            trapSiteDesc(), hugeMemoryEnabled(memoryIndex));
    auto* value = loadedValues.popCopy();
    if (!store(dst, &access, value)) {
      return false;
    }
  }

  if (numCopies2) {
    offset -= sizeof(uint16_t);

    MemoryAccessDesc access(memoryIndex, Scalar::Uint16, 1, offset,
                            trapSiteDesc(), hugeMemoryEnabled(memoryIndex));
    auto* value = loadedValues.popCopy();
    if (!store(dst, &access, value)) {
      return false;
    }
  }

  for (uint32_t i = 0; i < numCopies4; i++) {
    offset -= sizeof(uint32_t);

    MemoryAccessDesc access(memoryIndex, Scalar::Uint32, 1, offset,
                            trapSiteDesc(), hugeMemoryEnabled(memoryIndex));
    auto* value = loadedValues.popCopy();
    if (!store(dst, &access, value)) {
      return false;
    }
  }

#ifdef JS_64BIT
  for (uint32_t i = 0; i < numCopies8; i++) {
    offset -= sizeof(uint64_t);

    MemoryAccessDesc access(memoryIndex, Scalar::Int64, 1, offset,
                            trapSiteDesc(), hugeMemoryEnabled(memoryIndex));
    auto* value = loadedValues.popCopy();
    if (!store(dst, &access, value)) {
      return false;
    }
  }
#endif

#ifdef ENABLE_WASM_SIMD
  for (uint32_t i = 0; i < numCopies16; i++) {
    offset -= sizeof(V128);

    MemoryAccessDesc access(memoryIndex, Scalar::Simd128, 1, offset,
                            trapSiteDesc(), hugeMemoryEnabled(memoryIndex));
    auto* value = loadedValues.popCopy();
    if (!store(dst, &access, value)) {
      return false;
    }
  }
#endif

  return true;
}

bool FunctionCompiler::emitMemFillCall(uint32_t memoryIndex, MDefinition* start,
                                       MDefinition* val, MDefinition* len) {
  MDefinition* base = memoryBase(memoryIndex);

  uint32_t bytecodeOffset = readBytecodeOffset();
  const SymbolicAddressSignature& callee =
      (codeMeta().usesSharedMemory(memoryIndex)
           ? (isMem32(memoryIndex) ? SASigMemFillSharedM32
                                   : SASigMemFillSharedM64)
           : (isMem32(memoryIndex) ? SASigMemFillM32 : SASigMemFillM64));
  return emitInstanceCall4(bytecodeOffset, callee, start, val, len, base);
}

bool FunctionCompiler::emitMemFillInline(uint32_t memoryIndex,
                                         MDefinition* start, MDefinition* val,
                                         uint32_t length) {
  MOZ_ASSERT(length != 0 && length <= MaxInlineMemoryFillLength);
  uint32_t value = val->toConstant()->toInt32();

  // Compute the number of copies of each width we will need to do
  size_t remainder = length;
#ifdef ENABLE_WASM_SIMD
  size_t numCopies16 = 0;
  if (MacroAssembler::SupportsFastUnalignedFPAccesses()) {
    numCopies16 = remainder / sizeof(V128);
    remainder %= sizeof(V128);
  }
#endif
#ifdef JS_64BIT
  size_t numCopies8 = remainder / sizeof(uint64_t);
  remainder %= sizeof(uint64_t);
#endif
  size_t numCopies4 = remainder / sizeof(uint32_t);
  remainder %= sizeof(uint32_t);
  size_t numCopies2 = remainder / sizeof(uint16_t);
  remainder %= sizeof(uint16_t);
  size_t numCopies1 = remainder;

  // Generate splatted definitions for wider fills as needed
#ifdef ENABLE_WASM_SIMD
  MDefinition* val16 = numCopies16 ? constantV128(V128(value)) : nullptr;
#endif
#ifdef JS_64BIT
  MDefinition* val8 =
      numCopies8 ? constantI64(int64_t(SplatByteToUInt<uint64_t>(value, 8)))
                 : nullptr;
#endif
  MDefinition* val4 =
      numCopies4 ? constantI32(int32_t(SplatByteToUInt<uint32_t>(value, 4)))
                 : nullptr;
  MDefinition* val2 =
      numCopies2 ? constantI32(int32_t(SplatByteToUInt<uint32_t>(value, 2)))
                 : nullptr;

  // Store the fill value to the destination from high to low. We will trap
  // without writing anything on the first store if any dest byte is
  // out-of-bounds.
  size_t offset = length;

  if (numCopies1) {
    offset -= sizeof(uint8_t);

    MemoryAccessDesc access(memoryIndex, Scalar::Uint8, 1, offset,
                            trapSiteDesc(), hugeMemoryEnabled(memoryIndex));
    if (!store(start, &access, val)) {
      return false;
    }
  }

  if (numCopies2) {
    offset -= sizeof(uint16_t);

    MemoryAccessDesc access(memoryIndex, Scalar::Uint16, 1, offset,
                            trapSiteDesc(), hugeMemoryEnabled(memoryIndex));
    if (!store(start, &access, val2)) {
      return false;
    }
  }

  for (uint32_t i = 0; i < numCopies4; i++) {
    offset -= sizeof(uint32_t);

    MemoryAccessDesc access(memoryIndex, Scalar::Uint32, 1, offset,
                            trapSiteDesc(), hugeMemoryEnabled(memoryIndex));
    if (!store(start, &access, val4)) {
      return false;
    }
  }

#ifdef JS_64BIT
  for (uint32_t i = 0; i < numCopies8; i++) {
    offset -= sizeof(uint64_t);

    MemoryAccessDesc access(memoryIndex, Scalar::Int64, 1, offset,
                            trapSiteDesc(), hugeMemoryEnabled(memoryIndex));
    if (!store(start, &access, val8)) {
      return false;
    }
  }
#endif

#ifdef ENABLE_WASM_SIMD
  for (uint32_t i = 0; i < numCopies16; i++) {
    offset -= sizeof(V128);

    MemoryAccessDesc access(memoryIndex, Scalar::Simd128, 1, offset,
                            trapSiteDesc(), hugeMemoryEnabled(memoryIndex));
    if (!store(start, &access, val16)) {
      return false;
    }
  }
#endif

  return true;
}

#if ENABLE_WASM_MEMORY_CONTROL
#endif

//////////////////////////////////////////////////////////////////////////////
//
// Wide Arithmetic support

//////////////////////////////////////////////////////////////////////////////
//
// SIMD support

#ifdef ENABLE_WASM_SIMD
bool FunctionCompiler::emitConstSimd128() {
  V128 v128;
  if (!iter().readV128Const(&v128)) {
    return false;
  }

  return push(constantV128(v128));
}

bool FunctionCompiler::emitBinarySimd128(bool commutative, SimdOp op) {
  MDefinition* lhs;
  MDefinition* rhs;
  if (!iter().readBinary(ValType::V128, &lhs, &rhs)) {
    return false;
  }

  return push(binarySimd128(lhs, rhs, commutative, op));
}

bool FunctionCompiler::emitTernarySimd128(wasm::SimdOp op) {
  MDefinition* v0;
  MDefinition* v1;
  MDefinition* v2;
  if (!iter().readTernary(ValType::V128, &v0, &v1, &v2)) {
    return false;
  }

  return push(ternarySimd128(v0, v1, v2, op));
}

bool FunctionCompiler::emitShiftSimd128(SimdOp op) {
  MDefinition* lhs;
  MDefinition* rhs;
  if (!iter().readVectorShift(&lhs, &rhs)) {
    return false;
  }

  return push(shiftSimd128(lhs, rhs, op));
}

bool FunctionCompiler::emitSplatSimd128(ValType inType, SimdOp op) {
  MDefinition* src;
  if (!iter().readConversion(inType, ValType::V128, &src)) {
    return false;
  }

  return push(scalarToSimd128(src, op));
}

bool FunctionCompiler::emitUnarySimd128(SimdOp op) {
  MDefinition* src;
  if (!iter().readUnary(ValType::V128, &src)) {
    return false;
  }

  return push(unarySimd128(src, op));
}

bool FunctionCompiler::emitReduceSimd128(SimdOp op) {
  MDefinition* src;
  if (!iter().readConversion(ValType::V128, ValType::I32, &src)) {
    return false;
  }

  return push(reduceSimd128(src, op, ValType::I32));
}

bool FunctionCompiler::emitExtractLaneSimd128(ValType outType,
                                              uint32_t laneLimit, SimdOp op) {
  uint32_t laneIndex;
  MDefinition* src;
  if (!iter().readExtractLane(outType, laneLimit, &laneIndex, &src)) {
    return false;
  }

  return push(reduceSimd128(src, op, outType, laneIndex));
}

bool FunctionCompiler::emitReplaceLaneSimd128(ValType laneType,
                                              uint32_t laneLimit, SimdOp op) {
  uint32_t laneIndex;
  MDefinition* lhs;
  MDefinition* rhs;
  if (!iter().readReplaceLane(laneType, laneLimit, &laneIndex, &lhs, &rhs)) {
    return false;
  }

  return push(replaceLaneSimd128(lhs, rhs, laneIndex, op));
}

bool FunctionCompiler::emitShuffleSimd128() {
  MDefinition* v1;
  MDefinition* v2;
  V128 control;
  if (!iter().readVectorShuffle(&v1, &v2, &control)) {
    return false;
  }

  return push(shuffleSimd128(v1, v2, control));
}

bool FunctionCompiler::emitLoadSplatSimd128(Scalar::Type viewType,
                                            wasm::SimdOp splatOp) {
  LinearMemoryAddress<MDefinition*> addr;
  if (!iter().readLoadSplat(Scalar::byteSize(viewType), &addr)) {
    return false;
  }

  auto* ins = loadSplatSimd128(viewType, addr, splatOp);
  if (!inDeadCode() && !ins) {
    return false;
  }
  return push(ins);
}

bool FunctionCompiler::emitLoadExtendSimd128(wasm::SimdOp op) {
  LinearMemoryAddress<MDefinition*> addr;
  if (!iter().readLoadExtend(&addr)) {
    return false;
  }

  auto* ins = loadExtendSimd128(addr, op);
  if (!inDeadCode() && !ins) {
    return false;
  }
  return push(ins);
}

bool FunctionCompiler::emitLoadZeroSimd128(Scalar::Type viewType,
                                           size_t numBytes) {
  LinearMemoryAddress<MDefinition*> addr;
  if (!iter().readLoadSplat(numBytes, &addr)) {
    return false;
  }

  auto* ins = loadZeroSimd128(viewType, numBytes, addr);
  if (!inDeadCode() && !ins) {
    return false;
  }
  return push(ins);
}

bool FunctionCompiler::emitLoadLaneSimd128(uint32_t laneSize) {
  uint32_t laneIndex;
  MDefinition* src;
  LinearMemoryAddress<MDefinition*> addr;
  if (!iter().readLoadLane(laneSize, &addr, &laneIndex, &src)) {
    return false;
  }

  auto* ins = loadLaneSimd128(laneSize, addr, laneIndex, src);
  if (!inDeadCode() && !ins) {
    return false;
  }
  return push(ins);
}

bool FunctionCompiler::emitStoreLaneSimd128(uint32_t laneSize) {
  uint32_t laneIndex;
  MDefinition* src;
  LinearMemoryAddress<MDefinition*> addr;
  if (!iter().readStoreLane(laneSize, &addr, &laneIndex, &src)) {
    return false;
  }

  return storeLaneSimd128(laneSize, addr, laneIndex, src);
}

#endif  // ENABLE_WASM_SIMD

// Speculatively inline a call_refs that are likely to target the expected
// function index in this module. A fallback for if the actual callee is not
// any of the speculated expected callees is always generated. This leads to a
// control flow chain that is roughly:
//
// if (ref.func $expectedFuncIndex_1) == actualCalleeFunc:
//   (call_inline $expectedFuncIndex1)
// else if (ref.func $expectedFuncIndex_2) == actualCalleeFunc:
//   (call_inline $expectedFuncIndex2)
// ...
// else:
//   (call_ref actualCalleeFunc)
//
bool FunctionCompiler::emitSpeculativeInlineCallRef(
    uint32_t bytecodeOffset, const FuncType& funcType,
    CallRefHint expectedFuncIndices, MDefinition* actualCalleeFunc,
    const DefVector& args, DefVector* results) {
  // There must be at least one speculative target.
  MOZ_ASSERT(!expectedFuncIndices.empty());

  // Perform an up front null check on the callee function reference.
  actualCalleeFunc = refAsNonNull(actualCalleeFunc);
  if (!actualCalleeFunc) {
    return false;
  }

  constexpr size_t numElseBlocks = CallRefHint::NUM_ENTRIES + 1;
  Vector<MBasicBlock*, numElseBlocks, SystemAllocPolicy> elseBlocks;
  if (!elseBlocks.reserve(numElseBlocks)) {
    return false;
  }

  for (uint32_t i = 0; i < expectedFuncIndices.length(); i++) {
    uint32_t funcIndex = expectedFuncIndices.get(i);

    // Load the cached value of `ref.func $expectedFuncIndex` for comparing
    // against `actualCalleeFunc`. This cached value may be null if the
    // `ref.func` for the expected function has not been executed in this
    // runtime session.
    //
    // This is okay because we have done a null check on the `actualCalleeFunc`
    // already and so comparing it against a null expected callee func will
    // return false and fall back to the general case. This can only happen if
    // we've deserialized a cached module in a different session, and then run
    // the code without ever acquiring a reference to the expected function. In
    // that case, the expected callee could never be the target of this
    // call_ref, so performing the fallback path is the right thing to do
    // anyways.
    MDefinition* expectedCalleeFunc = loadCachedRefFunc(funcIndex);
    if (!expectedCalleeFunc) {
      return false;
    }

    // Check if the callee funcref we have is equals to the expected callee
    // funcref we're inlining.
    MDefinition* isExpectedCallee =
        compare(actualCalleeFunc, expectedCalleeFunc, JSOp::Eq,
                MCompare::Compare_WasmAnyRef);
    if (!isExpectedCallee) {
      return false;
    }

    // Start a 'then' block, which will have the inlined code
    MBasicBlock* elseBlock;
    if (!branchAndStartThen(isExpectedCallee, &elseBlock)) {
      return false;
    }

    // Inline the expected callee as we do with direct calls
    DefVector inlineResults;
    if (!emitInlineCall(funcType, funcIndex,
                        InliningHeuristics::CallKind::CallRef, args,
                        &inlineResults)) {
      return false;
    }

    // Push the results for joining with the 'else' block
    if (!pushDefs(inlineResults)) {
      return false;
    }

    // Switch to the 'else' block which will have, either the check for the
    // next target, or the fallback `call_ref` if we're out of targets.
    if (!switchToElse(elseBlock, &elseBlock)) {
      return false;
    }

    elseBlocks.infallibleAppend(elseBlock);
  }

  // The block that performs the fallback call should be cold.
  curBlock_->setFrequency(Frequency::Unlikely);

  DefVector callResults;
  if (!callRef(funcType, actualCalleeFunc, bytecodeOffset, args,
               &callResults)) {
    return false;
  }

  // Push the results for joining with the 'then' block
  if (!pushDefs(callResults)) {
    return false;
  }

  // Join the various branches together
  for (uint32_t i = elseBlocks.length() - 1; i != 0; i--) {
    DefVector results;
    if (!joinIfElse(elseBlocks[i], &results) || !pushDefs(results)) {
      return false;
    }
  }
  return joinIfElse(elseBlocks[0], results);
}

#ifdef ENABLE_WASM_JSPI
bool FunctionCompiler::emitContNew() {
  uint32_t typeIndex;
  MDefinition* func;
  if (!iter().readContNew(&typeIndex, &func)) {
    return false;
  }

  if (inDeadCode()) {
    return true;
  }

  // Load the type-specific base frame stub pointer from TypeDefInstanceData.
  uint32_t stubOffset = wasm::Instance::offsetInData(
      codeMeta().offsetOfContBaseFrameStub(typeIndex));
  auto* stub = MWasmLoadInstance::New(alloc(), instancePointer_, stubOffset,
                                      MIRType::Pointer, AliasSet::None());
  if (!stub) {
    return false;
  }
  curBlock_->add(stub);

  MDefinition* result = nullptr;
  if (!emitInstanceCall2(readBytecodeOffset(), SASigContNew, func, stub,
                         &result)) {
    return false;
  }
  iter().setResult(result);

  return true;
}

bool FunctionCompiler::emitContBind() {
  uint32_t inputContTypeIndex;
  uint32_t outputContTypeIndex;
  DefVector boundArgs;
  MDefinition* cont;
  if (!iter().readContBind(&inputContTypeIndex, &outputContTypeIndex,
                           &boundArgs, &cont)) {
    return false;
  }

  if (inDeadCode()) {
    return true;
  }

  // TODO: Not yet implemented
  unimplementedTrap();
  return true;
}

bool FunctionCompiler::emitStoreSuspendParams(
    MDefinition* paramsArea, const ValTypeVector& suspendTagParams,
    const DefVector& suspendParams, MDefinition* suspendedCont) {
  // Last param is the cont, it goes first in stack results.
  size_t paramsAreaOffset = 0;
  MWasmStackResultArea::StackResult loc(paramsAreaOffset,
                                        js::jit::MIRType::WasmAnyRef);
  MWasmStoreStackResult* storeCont = MWasmStoreStackResult::New(
      alloc(), paramsArea, paramsAreaOffset, suspendedCont);
  if (!storeCont) {
    return false;
  }
  curBlock_->add(storeCont);
  paramsAreaOffset = loc.endOffset();

  // The rest are suspendTagParams, again in reverse order.
  for (uint32_t i = 0; i < suspendTagParams.length(); i++) {
    if (!mirGen().ensureBallast()) {
      return false;
    }
    size_t reverseIndex = suspendTagParams.length() - i - 1;

    ValType handlerParam = suspendTagParams[reverseIndex];
    MWasmStackResultArea::StackResult loc(paramsAreaOffset,
                                          handlerParam.toMIRType());

    MDefinition* param = suspendParams[reverseIndex];
    MWasmStoreStackResult* store = MWasmStoreStackResult::New(
        alloc(), paramsArea, paramsAreaOffset, param);
    if (!store) {
      return false;
    }
    curBlock_->add(store);

    paramsAreaOffset = loc.endOffset();
  }
  return true;
}

// Always returns a (possibly empty) area, so switch nodes can hold it as a
// fixed operand. An empty area reserves no stack and byteSize() is 0.
bool FunctionCompiler::makeSwitchStackResultArea(const ValTypeVector& types,
                                                 MWasmStackResultArea** out) {
  *out = nullptr;
  auto* area = MWasmStackResultArea::New(alloc());
  if (!area || !area->init(alloc(), types.length())) {
    return false;
  }
  size_t offset = 0;
  for (uint32_t i = 0; i < types.length(); i++) {
    MWasmStackResultArea::StackResult loc(offset, types[i].toMIRType());
    area->initResult(i, loc);
    offset = loc.endOffset();
  }
  curBlock_->add(area);
  *out = area;
  return true;
}

bool FunctionCompiler::emitSuspend() {
  uint32_t tagIndex;
  DefVector suspendParams;
  if (!iter().readSuspend(&tagIndex, &suspendParams)) {
    return false;
  }

  if (inDeadCode()) {
    return true;
  }

  const TagDesc& tagDesc = codeMeta().tags[tagIndex];
  const TagType& tagType = *tagDesc.type;

  // TODO: Temporary restriction that we can't be in a try block yet. A
  // resume_throw will be able to trigger an exception that we need to handle.
  if (inTryCode()) {
    unimplementedTrap();
    return true;
  }

  // Load the tag we're going to search for.
  MDefinition* tag = loadTag(tagIndex);
  if (!tag) {
    return false;
  }

  // Search for the handler for this tag. When this suspend is used for JS-PI
  // this find handler will be infallible because we emit a different find
  // handler in emitGuardSuspending (which throws an exception instead of a
  // trap).
  MWasmFindHandler* handler =
      MWasmFindHandler::New(alloc(), instancePointer_, tag,
                            Trap::NullPointerDereference, trapSiteDesc());
  if (!handler) {
    return false;
  }
  curBlock_->add(handler);

  // Load the paramsArea from the handler.
  MWasmLoadInstance* paramsArea =
      MWasmLoadInstance::New(alloc(), handler,
                             offsetof(wasm::Handler, target) +
                                 offsetof(wasm::SwitchTarget, paramsArea),
                             MIRType::Pointer, AliasSet::None());
  if (!paramsArea) {
    return false;
  }
  curBlock_->add(paramsArea);

  // Allocate a new continuation that will hold our stack when we suspend.
  MDefinition* suspendedCont = nullptr;
  if (!emitInstanceCall0(readBytecodeOffset(), SASigContNewEmpty,
                         &suspendedCont)) {
    return false;
  }

  // Store all the params into the handler params area.
  if (!emitStoreSuspendParams(paramsArea, tagType.argTypes(), suspendParams,
                              suspendedCont)) {
    return false;
  }

  // The suspendResultsArea is always present (empty when the tag has no
  // results) so MWasmSuspend has a fixed operand set. After re-resume, the tag
  // results are read from it.
  const ValTypeVector& tagResults = tagType.resultTypes();
  MWasmStackResultArea* suspendResultsArea = nullptr;
  if (!makeSwitchStackResultArea(tagResults, &suspendResultsArea)) {
    return false;
  }

  // Emit the suspend instruction.
  MWasmSuspend* suspend = MWasmSuspend::New(
      alloc(), instancePointer_, suspendedCont, handler, suspendResultsArea,
      callSiteDesc(CallSiteKind::StackSwitch));
  if (!suspend) {
    return false;
  }
  curBlock_->add(suspend);

  // After re-resume, read tag results from the suspendResultsArea.
  if (!tagResults.empty()) {
    DefVector resultDefs;
    for (uint32_t i = 0; i < tagResults.length(); i++) {
      if (!mirGen().ensureBallast()) {
        return false;
      }
      MWasmStackResult* stackResult =
          MWasmStackResult::New(alloc(), suspendResultsArea, i);
      if (!stackResult || !resultDefs.append(stackResult)) {
        return false;
      }
      curBlock_->add(stackResult);
    }
    iter().setResults(resultDefs.length(), resultDefs);
  }

  return true;
}

bool FunctionCompiler::emitResume() {
  uint32_t typeIndex;
  MDefinition* cont;
  DefVector args;
  HandlerExprVector handlers;
  if (!iter().readResume(&typeIndex, &handlers, &args, &cont)) {
    return false;
  }

  if (inDeadCode()) {
    return true;
  }

  // TODO: Temporary restriction that suspend tags cannot have switch handlers.
  for (const HandlerExpr& handler : handlers) {
    if (handler.isSwitch()) {
      unimplementedTrap();
      return true;
    }
  }

  const TypeDef& contTypeDef = codeMeta().types->type(typeIndex);
  const FuncType& contFuncType = contTypeDef.contType().funcType();

  // Start with a resume barrier which will mark the stack if it hasn't yet and
  // we're in an incremental GC.
  MWasmResumeBarrier* barrier =
      MWasmResumeBarrier::New(alloc(), instancePointer_, cont);
  if (!barrier) {
    return false;
  }
  curBlock_->add(barrier);

  // Allocate the cont params area (empty when the cont takes none) and emit
  // MWasmPrepareResume unconditionally. It validates the continuation (trapping
  // if it is not resumable) and computes where the args go: the resumer's own
  // resumeParamsArea for a fresh continuation, or the suspender's advertised
  // suspendResultsArea for a suspended one, so the args are written straight
  // there without an extra copy. Emitting it for every resume also means the
  // resumability check happens once here rather than again in EmitResume.
  MWasmStackResultArea* resumeParamsArea = nullptr;
  if (!makeSwitchStackResultArea(contFuncType.args(), &resumeParamsArea)) {
    return false;
  }
  MWasmPrepareResume* prepareResume =
      MWasmPrepareResume::New(alloc(), cont, resumeParamsArea, trapSiteDesc());
  if (!prepareResume) {
    return false;
  }
  curBlock_->add(prepareResume);
  for (uint32_t i = 0; i < args.length(); i++) {
    if (!mirGen().ensureBallast()) {
      return false;
    }
    size_t argOffset = resumeParamsArea->result(i).offset();
    MWasmStoreStackResult* store =
        MWasmStoreStackResult::New(alloc(), prepareResume, argOffset, args[i]);
    if (!store) {
      return false;
    }
    curBlock_->add(store);
  }

  // If the cont has results, allocate a stack area for them so the typed base
  // frame stub can store them; results are read after the fallthrough.
  MWasmStackResultArea* contResultsArea = nullptr;
  if (!makeSwitchStackResultArea(contFuncType.results(), &contResultsArea)) {
    return false;
  }

  MBasicBlock* fallthroughBlock = nullptr;
  MBasicBlock* prePadBlock = nullptr;

  ControlInstructionVector* tryLandingPadPatches = nullptr;
  mozilla::Maybe<uint32_t> tryNote;
  if (inTryBlock(&tryLandingPadPatches)) {
    tryNote.emplace(0);
    if (!rootCompiler_.addTryNote(tryNote.ptr())) {
      return false;
    }
    if (!newBlock(curBlock_, &prePadBlock)) {
      return false;
    }
  }

  if (!newBlock(curBlock_, &fallthroughBlock)) {
    return false;
  }

  size_t numResultsAreaItems = 0;
  for (size_t i = 0; i < handlers.length(); i++) {
    const HandlerExpr& handler = handlers[i];
    MOZ_ASSERT(!handler.isSwitch());
    numResultsAreaItems +=
        1 + codeMeta().getTagType(handler.tagIndex()).argTypes().length();
  }

  // Always present (empty when the resume has no handlers) so MWasmResume has a
  // fixed operand set.
  MWasmStackResultArea* handlersResultArea = MWasmStackResultArea::New(alloc());
  if (!handlersResultArea ||
      !handlersResultArea->init(alloc(), numResultsAreaItems)) {
    return false;
  }
  curBlock_->add(handlersResultArea);

  MWasmResume* resume = MWasmResume::New(
      alloc(), callSiteDesc(CallSiteKind::StackSwitch), tryNote,
      instancePointer_, cont, handlersResultArea, contResultsArea);
  if (!resume ||
      !resume->init(fallthroughBlock, prePadBlock, handlers.length())) {
    return false;
  }
  curBlock_->end(resume);
  MBasicBlock* resumeBlock = curBlock_;
  curBlock_ = nullptr;

  // emitHandlerLandingPads
  size_t currentResultsAreaIndex = 0;
  size_t currentResultsAreaOffset = 0;
  for (size_t i = 0; i < handlers.length(); i++) {
    MOZ_ASSERT(!curBlock_);

    const HandlerExpr& handler = handlers[i];
    MOZ_ASSERT(!handler.isSwitch());

    uint32_t baseResultsAreaByteOffset = currentResultsAreaOffset;
    const TagDesc& tagDesc = codeMeta().tags[handler.tagIndex()];
    const TagType& tagType = *tagDesc.type;
    ResultType suspendTagParams = tagType.argResultType();

    // Last param is the cont, it goes first in stack results.
    MWasmStackResultArea::StackResult loc(currentResultsAreaOffset,
                                          js::jit::MIRType::WasmAnyRef);
    handlersResultArea->initResult(currentResultsAreaIndex, loc);
    currentResultsAreaIndex++;
    currentResultsAreaOffset = loc.endOffset();

    // The rest are suspendTagParams, again in reverse order.
    for (uint32_t i = 0; i < suspendTagParams.length(); i++) {
      size_t reverseIndex = suspendTagParams.length() - i - 1;
      ValType handlerParam = suspendTagParams[reverseIndex];

      MWasmStackResultArea::StackResult loc(currentResultsAreaOffset,
                                            handlerParam.toMIRType());
      handlersResultArea->initResult(currentResultsAreaIndex, loc);
      currentResultsAreaIndex++;
      currentResultsAreaOffset = loc.endOffset();
    }

    MBasicBlock* target;
    if (!newBlock(resumeBlock, &target)) {
      return false;
    }
    curBlock_ = target;

    if (!resume->initHandler(
            i, codeMeta().offsetOfTagInstanceData(handler.tagIndex()),
            baseResultsAreaByteOffset, target)) {
      return false;
    }

    DefVector resultDefs;
    size_t suspendLabelParams = suspendTagParams.length() + 1;
    for (uint32_t i = 0; i < suspendLabelParams; i++) {
      if (!mirGen().ensureBallast()) {
        return false;
      }
      size_t stackResultIndex = currentResultsAreaIndex - i - 1;

      MWasmStackResult* stackResult =
          MWasmStackResult::New(alloc(), handlersResultArea, stackResultIndex);
      if (!stackResult || !resultDefs.append(stackResult)) {
        return false;
      }
      curBlock_->add(stackResult);
    }

    if (!br(handler.labelDepth(), resultDefs)) {
      return false;
    }

    curBlock_ = nullptr;
  }

  MOZ_ASSERT(!curBlock_);
  if (tryNote.isSome()) {
    // Switch to the prePadBlock
    curBlock_ = prePadBlock;

    // Mark this as the landing pad for the call
    curBlock_->add(MWasmCallLandingPrePad::New(alloc(), resumeBlock, *tryNote));

    // End with a pending jump to the landing pad
    if (!endWithPadPatch(tryLandingPadPatches)) {
      return false;
    }
  }

  // Compilation continues in the fallthroughBlock.
  curBlock_ = fallthroughBlock;

  // Read cont results from contResultsArea into the value stack.
  if (!contFuncType.results().empty()) {
    size_t numResults = contFuncType.results().length();
    DefVector resultDefs;
    for (uint32_t i = 0; i < numResults; i++) {
      if (!mirGen().ensureBallast()) {
        return false;
      }
      MWasmStackResult* stackResult =
          MWasmStackResult::New(alloc(), contResultsArea, i);
      if (!stackResult || !resultDefs.append(stackResult)) {
        return false;
      }
      curBlock_->add(stackResult);
    }
    iter().setResults(resultDefs.length(), resultDefs);
  }

  return true;
}

bool FunctionCompiler::emitResumeThrow() {
  uint32_t typeIndex;
  uint32_t tagIndex;
  MDefinition* cont;
  DefVector args;
  HandlerExprVector handlers;
  if (!iter().readResumeThrow(&typeIndex, &tagIndex, &handlers, &args, &cont)) {
    return false;
  }

  if (inDeadCode()) {
    return true;
  }

  // TODO: Not yet implemented.
  unimplementedTrap();
  return true;
}

bool FunctionCompiler::emitResumeThrowRef() {
  uint32_t contTypeIndex;
  MDefinition* cont;
  MDefinition* exception;
  HandlerExprVector handlers;
  if (!iter().readResumeThrowRef(&contTypeIndex, &handlers, &exception,
                                 &cont)) {
    return false;
  }

  if (inDeadCode()) {
    return true;
  }

  // TODO: Not yet implemented.
  unimplementedTrap();
  return true;
}

bool FunctionCompiler::emitSwitch() {
  uint32_t contTypeIndex;
  uint32_t tagIndex;
  MDefinition* cont;
  DefVector args;
  HandlerExprVector handlers;
  if (!iter().readSwitch(&contTypeIndex, &tagIndex, &args, &cont)) {
    return false;
  }

  if (inDeadCode()) {
    return true;
  }

  // TODO: Not yet implemented.
  unimplementedTrap();
  return true;
}

bool FunctionCompiler::emitGuardSuspending() {
  uint32_t tagIndex;
  if (!iter().readGuardSuspending(&tagIndex)) {
    return false;
  }

  if (inDeadCode()) {
    return true;
  }

  MDefinition* tag = loadTag(tagIndex);
  if (!tag) {
    return false;
  }

  // This will throw an exception (not a trap) which would require us to add
  // branches to catch blocks. However this is only used in self-hosted wasm
  // code for JS-PI where we ensure there is no try blocks.
  MOZ_ASSERT(!inTryCode());

  MWasmFindHandler* handler = MWasmFindHandler::New(
      alloc(), instancePointer_, tag, Trap::ThrowSuspendError, trapSiteDesc());
  if (!handler) {
    return false;
  }
  curBlock_->add(handler);

  return true;
}

#endif  // ENABLE_WASM_JSPI

bool FunctionCompiler::beginPushedBody() {
  MOZ_ASSERT(controlStack_.empty());
  return pushControl(LabelKind::Body, BlockType::FuncResults(funcType()));
}

bool FunctionCompiler::beginPushedOp(OpBytes op, size_t opOffset) {
  if (!mirGen().ensureBallast()) {
    return false;
  }
  // There is no readOp to record this: the validator read the operator, and
  // where it started is what trap sites want.
  currentOpOffset_ = opOffset;
  return true;
}

}  // end anonymous namespace

// Ion's opcode-to-emitter mapping, driven from itasca.
//
// The same shape as baseline's: each hook names its own opcode and calls
// straight into the one method that emits it, so there is no switch behind
// the call.

namespace {

// Everything the hooks share for one function body.
struct IonDriver {
  FunctionCompiler& f;
  // See BaseDriver::opOffsetBase.
  uint32_t opOffsetBase = 0;
  // The function's locals, read by on_function_start out of the VwContext
  // before it calls f.init(locals).
  ValTypeVector locals;
  // A br_table's labels arrive one hook at a time and are used by the hook
  // that closes the operator.
  Uint32Vector brTableDepths;
  DeclineReason decline = DeclineReason::None;
};

IonDriver* driver(const VwHookCtx* c) {
  return static_cast<IonDriver*>(c->user);
}

}  // namespace

#define ION_OP_BEGIN(c, opcode)                        \
  IonDriver* st = driver(c);                           \
  FunctionCompiler& f = st->f;                         \
  size_t opOffset = st->opOffsetBase + (c)->op_offset; \
  if (!f.beginPushedOp(OpBytes(opcode), opOffset)) {   \
    st->decline = DeclineReason::OutOfMemory;          \
    return VW_VISIT_OOM;                               \
  }

// The epilogue: run the operator's own emitter and translate its result.
#define ION_OP_END(call)                      \
  if (!(call)) {                              \
    st->decline = DeclineReason::OutOfMemory; \
    return VW_VISIT_OOM;                      \
  }                                           \
  return VW_VISIT_OK;

// The hooks. Each names its own opcode and calls the one method that maps to
// it, so this is the whole of the dispatch: there is no second switch behind
// it.
struct IonItascaHooks {
  static uint32_t on_function_start(const VwHookCtx* c, const VwContext* ctx,
                                    uint32_t, size_t, size_t) {
    IonDriver* st = driver(c);

    // MaxLocals counts a function's parameters, where itasca's cap is on the
    // declarations alone.
    size_t numLocals = itasca_context_num_locals(ctx);
    if (numLocals > MaxLocals) {
      st->decline = DeclineReason::TooManyLocals;
      return VW_VISIT_OOM;
    }
    if (!st->locals.reserve(numLocals)) {
      st->decline = DeclineReason::OutOfMemory;
      return VW_VISIT_OOM;
    }
    for (size_t i = 0; i < numLocals; i++) {
      st->locals.infallibleAppend(
          ToWasmValType(itasca_context_local(ctx, i)));
    }

    FunctionCompiler& f = st->f;
    f.rootCompiler().setWasmNumLocals(uint32_t(numLocals));
    f.init(st->locals);
    if (!f.initRoot() || !f.startBlock() || !f.beginPushedBody()) {
      st->decline = DeclineReason::OutOfMemory;
      return VW_VISIT_OOM;
    }
    return VW_VISIT_OK;
  }

  static uint32_t on_block(const VwHookCtx* c, const VwState*,
                           VwBlockType type) {
    ION_OP_BEGIN(c, Op::Block);
    ION_OP_END(f.emitBlock(ToWasmBlockType(type)));
  }

  static uint32_t on_loop(const VwHookCtx* c, const VwState*,
                          VwBlockType type) {
    ION_OP_BEGIN(c, Op::Loop);
    ION_OP_END(f.emitLoop(ToWasmBlockType(type)));
  }

  static uint32_t on_if(const VwHookCtx* c, const VwState*, VwBlockType type) {
    ION_OP_BEGIN(c, Op::If);
    ION_OP_END(f.emitIf(ToWasmBlockType(type)));
  }

  static uint32_t on_else(const VwHookCtx* c, const VwState*, VwBlockType) {
    ION_OP_BEGIN(c, Op::Else);
    ION_OP_END(f.emitElse());
  }

  static uint32_t on_end(const VwHookCtx* c, const VwState*, VwLabelKind,
                         VwBlockType) {
    ION_OP_BEGIN(c, Op::End);
    ION_OP_END(f.emitEnd());
  }

  static uint32_t on_br(const VwHookCtx* c, const VwState*, uint32_t depth,
                        VwBlockType) {
    ION_OP_BEGIN(c, Op::Br);
    ION_OP_END(f.emitBr(depth));
  }

  static uint32_t on_br_if(const VwHookCtx* c, const VwState*, uint32_t depth,
                           VwBlockType) {
    ION_OP_BEGIN(c, Op::BrIf);
    ION_OP_END(f.emitBrIf(depth));
  }

  // A br_table's labels arrive one at a time, and no hook says how many there
  // will be, so they are collected until the operator closes.
  static uint32_t on_br_table_label(const VwHookCtx* c, const VwState*,
                                    uint32_t depth) {
    IonDriver* st = driver(c);
    if (st->brTableDepths.length() >= MaxBrTableElems) {
      st->decline = DeclineReason::BrTableTooBig;
      return VW_VISIT_OOM;
    }
    if (!st->brTableDepths.append(depth)) {
      st->decline = DeclineReason::OutOfMemory;
      return VW_VISIT_OOM;
    }
    return VW_VISIT_OK;
  }

  static uint32_t on_br_table(const VwHookCtx* c, const VwState*,
                              uint32_t defaultDepth, VwBlockType) {
    ION_OP_BEGIN(c, Op::BrTable);
    if (!f.emitBrTable(st->brTableDepths, defaultDepth)) {
      st->decline = DeclineReason::OutOfMemory;
      return VW_VISIT_OOM;
    }
    st->brTableDepths.clear();
    return VW_VISIT_OK;
  }

  static uint32_t on_drop(const VwHookCtx* c, const VwState*, VwStackType) {
    ION_OP_BEGIN(c, Op::Drop);
    f.pop();
    ION_OP_END(true);
  }

  static uint32_t on_select(const VwHookCtx* c, const VwState*, VwStackType) {
    ION_OP_BEGIN(c, Op::SelectNumeric);
    ION_OP_END(f.emitSelect(/*typed*/ false));
  }

  static uint32_t on_i32_const(const VwHookCtx* c, const VwState*,
                               int32_t value) {
    ION_OP_BEGIN(c, Op::I32Const);
    ION_OP_END(f.emitI32Const(value));
  }

  static uint32_t on_i64_const(const VwHookCtx* c, const VwState*,
                               int64_t value) {
    ION_OP_BEGIN(c, Op::I64Const);
    ION_OP_END(f.emitI64Const(value));
  }

  static uint32_t on_f32_const(const VwHookCtx* c, const VwState*,
                               uint32_t bits) {
    ION_OP_BEGIN(c, Op::F32Const);
    float value;
    memcpy(&value, &bits, sizeof(value));
    ION_OP_END(f.emitF32Const(value));
  }

  static uint32_t on_f64_const(const VwHookCtx* c, const VwState*,
                               uint64_t bits) {
    ION_OP_BEGIN(c, Op::F64Const);
    double value;
    memcpy(&value, &bits, sizeof(value));
    ION_OP_END(f.emitF64Const(value));
  }

  static uint32_t on_unreachable(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::Unreachable);
    ION_OP_END(f.emitUnreachable());
  }

  static uint32_t on_nop(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::Nop);
    ION_OP_END(true);
  }

  static uint32_t on_return(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::Return);
    ION_OP_END(f.emitReturn());
  }

  static uint32_t on_memory_size(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::MemorySize);
    ION_OP_END(f.emitMemorySize());
  }

  static uint32_t on_memory_grow(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::MemoryGrow);
    ION_OP_END(f.emitMemoryGrow());
  }

  static uint32_t on_call(const VwHookCtx* c, const VwState*, uint32_t index) {
    ION_OP_BEGIN(c, Op::Call);
    ION_OP_END(f.emitCall(index));
  }

  static uint32_t on_call_indirect(const VwHookCtx* c, const VwState*,
                                   uint32_t index) {
    ION_OP_BEGIN(c, Op::CallIndirect);
    ION_OP_END(f.emitCallIndirect(index));
  }

  static uint32_t on_local_get(const VwHookCtx* c, const VwState*,
                               uint32_t index) {
    ION_OP_BEGIN(c, Op::LocalGet);
    ION_OP_END(f.emitGetLocal(index));
  }

  static uint32_t on_local_set(const VwHookCtx* c, const VwState*,
                               uint32_t index) {
    ION_OP_BEGIN(c, Op::LocalSet);
    ION_OP_END(f.emitSetLocal(index));
  }

  static uint32_t on_local_tee(const VwHookCtx* c, const VwState*,
                               uint32_t index) {
    ION_OP_BEGIN(c, Op::LocalTee);
    ION_OP_END(f.emitTeeLocal(index));
  }

  static uint32_t on_global_get(const VwHookCtx* c, const VwState*,
                                uint32_t index) {
    ION_OP_BEGIN(c, Op::GlobalGet);
    ION_OP_END(f.emitGetGlobal(index));
  }

  static uint32_t on_global_set(const VwHookCtx* c, const VwState*,
                                uint32_t index) {
    ION_OP_BEGIN(c, Op::GlobalSet);
    ION_OP_END(f.emitSetGlobal(index));
  }

  // The memory hooks: each names the (ValType, Scalar::Type) its opcode loads
  // or stores. Generated from the mnemonics in SM_VW_FOR_EACH_MEMORY_OP, so a
  // hook cannot be wired to the wrong shape; written out rather than
  // table-generated because each needs different arguments to emitLoad or
  // emitStore, which the two-column (hook, opcode) table does not carry.
  static uint32_t on_i32_load(const VwHookCtx* c, const VwState*, VwMemArg m) {
    ION_OP_BEGIN(c, Op::I32Load);
    ION_OP_END(f.emitLoad(ValType::I32, Scalar::Int32, m.align, m.offset));
  }

  static uint32_t on_i64_load(const VwHookCtx* c, const VwState*, VwMemArg m) {
    ION_OP_BEGIN(c, Op::I64Load);
    ION_OP_END(f.emitLoad(ValType::I64, Scalar::Int64, m.align, m.offset));
  }

  static uint32_t on_f32_load(const VwHookCtx* c, const VwState*, VwMemArg m) {
    ION_OP_BEGIN(c, Op::F32Load);
    ION_OP_END(f.emitLoad(ValType::F32, Scalar::Float32, m.align, m.offset));
  }

  static uint32_t on_f64_load(const VwHookCtx* c, const VwState*, VwMemArg m) {
    ION_OP_BEGIN(c, Op::F64Load);
    ION_OP_END(f.emitLoad(ValType::F64, Scalar::Float64, m.align, m.offset));
  }

  static uint32_t on_i32_load8_s(const VwHookCtx* c, const VwState*,
                                 VwMemArg m) {
    ION_OP_BEGIN(c, Op::I32Load8S);
    ION_OP_END(f.emitLoad(ValType::I32, Scalar::Int8, m.align, m.offset));
  }

  static uint32_t on_i32_load8_u(const VwHookCtx* c, const VwState*,
                                 VwMemArg m) {
    ION_OP_BEGIN(c, Op::I32Load8U);
    ION_OP_END(f.emitLoad(ValType::I32, Scalar::Uint8, m.align, m.offset));
  }

  static uint32_t on_i32_load16_s(const VwHookCtx* c, const VwState*,
                                  VwMemArg m) {
    ION_OP_BEGIN(c, Op::I32Load16S);
    ION_OP_END(f.emitLoad(ValType::I32, Scalar::Int16, m.align, m.offset));
  }

  static uint32_t on_i32_load16_u(const VwHookCtx* c, const VwState*,
                                  VwMemArg m) {
    ION_OP_BEGIN(c, Op::I32Load16U);
    ION_OP_END(f.emitLoad(ValType::I32, Scalar::Uint16, m.align, m.offset));
  }

  static uint32_t on_i64_load8_s(const VwHookCtx* c, const VwState*,
                                 VwMemArg m) {
    ION_OP_BEGIN(c, Op::I64Load8S);
    ION_OP_END(f.emitLoad(ValType::I64, Scalar::Int8, m.align, m.offset));
  }

  static uint32_t on_i64_load8_u(const VwHookCtx* c, const VwState*,
                                 VwMemArg m) {
    ION_OP_BEGIN(c, Op::I64Load8U);
    ION_OP_END(f.emitLoad(ValType::I64, Scalar::Uint8, m.align, m.offset));
  }

  static uint32_t on_i64_load16_s(const VwHookCtx* c, const VwState*,
                                  VwMemArg m) {
    ION_OP_BEGIN(c, Op::I64Load16S);
    ION_OP_END(f.emitLoad(ValType::I64, Scalar::Int16, m.align, m.offset));
  }

  static uint32_t on_i64_load16_u(const VwHookCtx* c, const VwState*,
                                  VwMemArg m) {
    ION_OP_BEGIN(c, Op::I64Load16U);
    ION_OP_END(f.emitLoad(ValType::I64, Scalar::Uint16, m.align, m.offset));
  }

  static uint32_t on_i64_load32_s(const VwHookCtx* c, const VwState*,
                                  VwMemArg m) {
    ION_OP_BEGIN(c, Op::I64Load32S);
    ION_OP_END(f.emitLoad(ValType::I64, Scalar::Int32, m.align, m.offset));
  }

  static uint32_t on_i64_load32_u(const VwHookCtx* c, const VwState*,
                                  VwMemArg m) {
    ION_OP_BEGIN(c, Op::I64Load32U);
    ION_OP_END(f.emitLoad(ValType::I64, Scalar::Uint32, m.align, m.offset));
  }

  static uint32_t on_i32_store(const VwHookCtx* c, const VwState*, VwMemArg m) {
    ION_OP_BEGIN(c, Op::I32Store);
    ION_OP_END(f.emitStore(ValType::I32, Scalar::Int32, m.align, m.offset));
  }

  static uint32_t on_i64_store(const VwHookCtx* c, const VwState*, VwMemArg m) {
    ION_OP_BEGIN(c, Op::I64Store);
    ION_OP_END(f.emitStore(ValType::I64, Scalar::Int64, m.align, m.offset));
  }

  static uint32_t on_f32_store(const VwHookCtx* c, const VwState*, VwMemArg m) {
    ION_OP_BEGIN(c, Op::F32Store);
    ION_OP_END(f.emitStore(ValType::F32, Scalar::Float32, m.align, m.offset));
  }

  static uint32_t on_f64_store(const VwHookCtx* c, const VwState*, VwMemArg m) {
    ION_OP_BEGIN(c, Op::F64Store);
    ION_OP_END(f.emitStore(ValType::F64, Scalar::Float64, m.align, m.offset));
  }

  static uint32_t on_i32_store8(const VwHookCtx* c, const VwState*,
                                VwMemArg m) {
    ION_OP_BEGIN(c, Op::I32Store8);
    ION_OP_END(f.emitStore(ValType::I32, Scalar::Int8, m.align, m.offset));
  }

  static uint32_t on_i32_store16(const VwHookCtx* c, const VwState*,
                                 VwMemArg m) {
    ION_OP_BEGIN(c, Op::I32Store16);
    ION_OP_END(f.emitStore(ValType::I32, Scalar::Int16, m.align, m.offset));
  }

  static uint32_t on_i64_store8(const VwHookCtx* c, const VwState*,
                                VwMemArg m) {
    ION_OP_BEGIN(c, Op::I64Store8);
    ION_OP_END(f.emitStore(ValType::I64, Scalar::Int8, m.align, m.offset));
  }

  static uint32_t on_i64_store16(const VwHookCtx* c, const VwState*,
                                 VwMemArg m) {
    ION_OP_BEGIN(c, Op::I64Store16);
    ION_OP_END(f.emitStore(ValType::I64, Scalar::Int16, m.align, m.offset));
  }

  static uint32_t on_i64_store32(const VwHookCtx* c, const VwState*,
                                 VwMemArg m) {
    ION_OP_BEGIN(c, Op::I64Store32);
    ION_OP_END(f.emitStore(ValType::I64, Scalar::Int32, m.align, m.offset));
  }

  // The numeric hooks: one emitter call per opcode, extracted from the old
  // switch so a hook cannot be mapped to the wrong operator.
  static uint32_t on_i32_eqz(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::I32Eqz);
    ION_OP_END(f.emitConversion<MNot>(ValType::I32, ValType::I32));
  }

  static uint32_t on_i32_eq(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::I32Eq);
    ION_OP_END(
        f.emitComparison(ValType::I32, JSOp::Eq, MCompare::Compare_Int32));
  }

  static uint32_t on_i32_ne(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::I32Ne);
    ION_OP_END(
        f.emitComparison(ValType::I32, JSOp::Ne, MCompare::Compare_Int32));
  }

  static uint32_t on_i32_lt_s(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::I32LtS);
    ION_OP_END(
        f.emitComparison(ValType::I32, JSOp::Lt, MCompare::Compare_Int32));
  }

  static uint32_t on_i32_lt_u(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::I32LtU);
    ION_OP_END(
        f.emitComparison(ValType::I32, JSOp::Lt, MCompare::Compare_UInt32));
  }

  static uint32_t on_i32_gt_s(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::I32GtS);
    ION_OP_END(
        f.emitComparison(ValType::I32, JSOp::Gt, MCompare::Compare_Int32));
  }

  static uint32_t on_i32_gt_u(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::I32GtU);
    ION_OP_END(
        f.emitComparison(ValType::I32, JSOp::Gt, MCompare::Compare_UInt32));
  }

  static uint32_t on_i32_le_s(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::I32LeS);
    ION_OP_END(
        f.emitComparison(ValType::I32, JSOp::Le, MCompare::Compare_Int32));
  }

  static uint32_t on_i32_le_u(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::I32LeU);
    ION_OP_END(
        f.emitComparison(ValType::I32, JSOp::Le, MCompare::Compare_UInt32));
  }

  static uint32_t on_i32_ge_s(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::I32GeS);
    ION_OP_END(
        f.emitComparison(ValType::I32, JSOp::Ge, MCompare::Compare_Int32));
  }

  static uint32_t on_i32_ge_u(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::I32GeU);
    ION_OP_END(
        f.emitComparison(ValType::I32, JSOp::Ge, MCompare::Compare_UInt32));
  }

  static uint32_t on_i64_eqz(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::I64Eqz);
    ION_OP_END(f.emitConversion<MNot>(ValType::I64, ValType::I32));
  }

  static uint32_t on_i64_eq(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::I64Eq);
    ION_OP_END(
        f.emitComparison(ValType::I64, JSOp::Eq, MCompare::Compare_Int64));
  }

  static uint32_t on_i64_ne(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::I64Ne);
    ION_OP_END(
        f.emitComparison(ValType::I64, JSOp::Ne, MCompare::Compare_Int64));
  }

  static uint32_t on_i64_lt_s(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::I64LtS);
    ION_OP_END(
        f.emitComparison(ValType::I64, JSOp::Lt, MCompare::Compare_Int64));
  }

  static uint32_t on_i64_lt_u(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::I64LtU);
    ION_OP_END(
        f.emitComparison(ValType::I64, JSOp::Lt, MCompare::Compare_UInt64));
  }

  static uint32_t on_i64_gt_s(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::I64GtS);
    ION_OP_END(
        f.emitComparison(ValType::I64, JSOp::Gt, MCompare::Compare_Int64));
  }

  static uint32_t on_i64_gt_u(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::I64GtU);
    ION_OP_END(
        f.emitComparison(ValType::I64, JSOp::Gt, MCompare::Compare_UInt64));
  }

  static uint32_t on_i64_le_s(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::I64LeS);
    ION_OP_END(
        f.emitComparison(ValType::I64, JSOp::Le, MCompare::Compare_Int64));
  }

  static uint32_t on_i64_le_u(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::I64LeU);
    ION_OP_END(
        f.emitComparison(ValType::I64, JSOp::Le, MCompare::Compare_UInt64));
  }

  static uint32_t on_i64_ge_s(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::I64GeS);
    ION_OP_END(
        f.emitComparison(ValType::I64, JSOp::Ge, MCompare::Compare_Int64));
  }

  static uint32_t on_i64_ge_u(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::I64GeU);
    ION_OP_END(
        f.emitComparison(ValType::I64, JSOp::Ge, MCompare::Compare_UInt64));
  }

  static uint32_t on_f32_eq(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::F32Eq);
    ION_OP_END(
        f.emitComparison(ValType::F32, JSOp::Eq, MCompare::Compare_Float32));
  }

  static uint32_t on_f32_ne(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::F32Ne);
    ION_OP_END(
        f.emitComparison(ValType::F32, JSOp::Ne, MCompare::Compare_Float32));
  }

  static uint32_t on_f32_lt(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::F32Lt);
    ION_OP_END(
        f.emitComparison(ValType::F32, JSOp::Lt, MCompare::Compare_Float32));
  }

  static uint32_t on_f32_gt(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::F32Gt);
    ION_OP_END(
        f.emitComparison(ValType::F32, JSOp::Gt, MCompare::Compare_Float32));
  }

  static uint32_t on_f32_le(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::F32Le);
    ION_OP_END(
        f.emitComparison(ValType::F32, JSOp::Le, MCompare::Compare_Float32));
  }

  static uint32_t on_f32_ge(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::F32Ge);
    ION_OP_END(
        f.emitComparison(ValType::F32, JSOp::Ge, MCompare::Compare_Float32));
  }

  static uint32_t on_f64_eq(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::F64Eq);
    ION_OP_END(
        f.emitComparison(ValType::F64, JSOp::Eq, MCompare::Compare_Double));
  }

  static uint32_t on_f64_ne(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::F64Ne);
    ION_OP_END(
        f.emitComparison(ValType::F64, JSOp::Ne, MCompare::Compare_Double));
  }

  static uint32_t on_f64_lt(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::F64Lt);
    ION_OP_END(
        f.emitComparison(ValType::F64, JSOp::Lt, MCompare::Compare_Double));
  }

  static uint32_t on_f64_gt(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::F64Gt);
    ION_OP_END(
        f.emitComparison(ValType::F64, JSOp::Gt, MCompare::Compare_Double));
  }

  static uint32_t on_f64_le(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::F64Le);
    ION_OP_END(
        f.emitComparison(ValType::F64, JSOp::Le, MCompare::Compare_Double));
  }

  static uint32_t on_f64_ge(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::F64Ge);
    ION_OP_END(
        f.emitComparison(ValType::F64, JSOp::Ge, MCompare::Compare_Double));
  }

  static uint32_t on_i32_sub(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::I32Sub);
    ION_OP_END(f.emitSub(ValType::I32, MIRType::Int32));
  }

  static uint32_t on_i32_mul(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::I32Mul);
    ION_OP_END(f.emitMul(ValType::I32, MIRType::Int32));
  }

  static uint32_t on_i32_div_s(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::I32DivS);
    ION_OP_END(f.emitDiv(ValType::I32, MIRType::Int32, false));
  }

  static uint32_t on_i32_div_u(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::I32DivU);
    ION_OP_END(f.emitDiv(ValType::I32, MIRType::Int32, true));
  }

  static uint32_t on_i32_rem_s(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::I32RemS);
    ION_OP_END(f.emitRem(ValType::I32, MIRType::Int32, false));
  }

  static uint32_t on_i32_rem_u(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::I32RemU);
    ION_OP_END(f.emitRem(ValType::I32, MIRType::Int32, true));
  }

  static uint32_t on_i32_and(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::I32And);
    ION_OP_END(f.emitBitwiseAndOrXor(ValType::I32, MIRType::Int32,
                                     MWasmBinaryBitwise::SubOpcode::And));
  }

  static uint32_t on_i32_or(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::I32Or);
    ION_OP_END(f.emitBitwiseAndOrXor(ValType::I32, MIRType::Int32,
                                     MWasmBinaryBitwise::SubOpcode::Or));
  }

  static uint32_t on_i32_xor(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::I32Xor);
    ION_OP_END(f.emitBitwiseAndOrXor(ValType::I32, MIRType::Int32,
                                     MWasmBinaryBitwise::SubOpcode::Xor));
  }

  static uint32_t on_i32_shl(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::I32Shl);
    ION_OP_END(f.emitShift<MLsh>(ValType::I32, MIRType::Int32));
  }

  static uint32_t on_i32_shr_s(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::I32ShrS);
    ION_OP_END(f.emitShift<MRsh>(ValType::I32, MIRType::Int32));
  }

  static uint32_t on_i32_shr_u(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::I32ShrU);
    ION_OP_END(f.emitUrsh(ValType::I32, MIRType::Int32));
  }

  static uint32_t on_i32_rotl(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::I32Rotl);
    ION_OP_END(f.emitRotate(ValType::I32, true));
  }

  static uint32_t on_i32_rotr(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::I32Rotr);
    ION_OP_END(f.emitRotate(ValType::I32, false));
  }

  static uint32_t on_i64_add(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::I64Add);
    ION_OP_END(f.emitAdd(ValType::I64, MIRType::Int64));
  }

  static uint32_t on_i64_sub(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::I64Sub);
    ION_OP_END(f.emitSub(ValType::I64, MIRType::Int64));
  }

  static uint32_t on_i64_mul(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::I64Mul);
    ION_OP_END(f.emitMul(ValType::I64, MIRType::Int64));
  }

  static uint32_t on_i64_div_s(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::I64DivS);
    ION_OP_END(f.emitDiv(ValType::I64, MIRType::Int64, false));
  }

  static uint32_t on_i64_div_u(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::I64DivU);
    ION_OP_END(f.emitDiv(ValType::I64, MIRType::Int64, true));
  }

  static uint32_t on_i64_rem_s(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::I64RemS);
    ION_OP_END(f.emitRem(ValType::I64, MIRType::Int64, false));
  }

  static uint32_t on_i64_rem_u(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::I64RemU);
    ION_OP_END(f.emitRem(ValType::I64, MIRType::Int64, true));
  }

  static uint32_t on_i64_and(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::I64And);
    ION_OP_END(f.emitBitwiseAndOrXor(ValType::I64, MIRType::Int64,
                                     MWasmBinaryBitwise::SubOpcode::And));
  }

  static uint32_t on_i64_or(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::I64Or);
    ION_OP_END(f.emitBitwiseAndOrXor(ValType::I64, MIRType::Int64,
                                     MWasmBinaryBitwise::SubOpcode::Or));
  }

  static uint32_t on_i64_xor(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::I64Xor);
    ION_OP_END(f.emitBitwiseAndOrXor(ValType::I64, MIRType::Int64,
                                     MWasmBinaryBitwise::SubOpcode::Xor));
  }

  static uint32_t on_i64_shl(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::I64Shl);
    ION_OP_END(f.emitShift<MLsh>(ValType::I64, MIRType::Int64));
  }

  static uint32_t on_i64_shr_s(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::I64ShrS);
    ION_OP_END(f.emitShift<MRsh>(ValType::I64, MIRType::Int64));
  }

  static uint32_t on_i64_shr_u(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::I64ShrU);
    ION_OP_END(f.emitUrsh(ValType::I64, MIRType::Int64));
  }

  static uint32_t on_i64_rotl(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::I64Rotl);
    ION_OP_END(f.emitRotate(ValType::I64, true));
  }

  static uint32_t on_i64_rotr(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::I64Rotr);
    ION_OP_END(f.emitRotate(ValType::I64, false));
  }

  static uint32_t on_f32_add(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::F32Add);
    ION_OP_END(f.emitAdd(ValType::F32, MIRType::Float32));
  }

  static uint32_t on_f32_sub(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::F32Sub);
    ION_OP_END(f.emitSub(ValType::F32, MIRType::Float32));
  }

  static uint32_t on_f32_mul(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::F32Mul);
    ION_OP_END(f.emitMul(ValType::F32, MIRType::Float32));
  }

  static uint32_t on_f32_div(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::F32Div);
    ION_OP_END(
        f.emitDiv(ValType::F32, MIRType::Float32, /* isUnsigned = */ false));
  }

  static uint32_t on_f32_min(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::F32Min);
    ION_OP_END(f.emitMinMax(ValType::F32, MIRType::Float32, false));
  }

  static uint32_t on_f32_max(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::F32Max);
    ION_OP_END(f.emitMinMax(ValType::F32, MIRType::Float32, true));
  }

  static uint32_t on_f32_copysign(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::F32CopySign);
    ION_OP_END(f.emitCopySign(ValType::F32));
  }

  static uint32_t on_f64_add(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::F64Add);
    ION_OP_END(f.emitAdd(ValType::F64, MIRType::Double));
  }

  static uint32_t on_f64_sub(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::F64Sub);
    ION_OP_END(f.emitSub(ValType::F64, MIRType::Double));
  }

  static uint32_t on_f64_mul(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::F64Mul);
    ION_OP_END(f.emitMul(ValType::F64, MIRType::Double));
  }

  static uint32_t on_f64_div(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::F64Div);
    ION_OP_END(
        f.emitDiv(ValType::F64, MIRType::Double, /* isUnsigned = */ false));
  }

  static uint32_t on_f64_min(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::F64Min);
    ION_OP_END(f.emitMinMax(ValType::F64, MIRType::Double, false));
  }

  static uint32_t on_f64_max(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::F64Max);
    ION_OP_END(f.emitMinMax(ValType::F64, MIRType::Double, true));
  }

  static uint32_t on_f64_copysign(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::F64CopySign);
    ION_OP_END(f.emitCopySign(ValType::F64));
  }

  static uint32_t on_i32_clz(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::I32Clz);
    ION_OP_END(f.emitUnaryWithType<MClz>(ValType::I32, MIRType::Int32));
  }

  static uint32_t on_i32_ctz(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::I32Ctz);
    ION_OP_END(f.emitUnaryWithType<MCtz>(ValType::I32, MIRType::Int32));
  }

  static uint32_t on_i32_popcnt(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::I32Popcnt);
    ION_OP_END(f.emitUnaryWithType<MPopcnt>(ValType::I32, MIRType::Int32));
  }

  static uint32_t on_i32_add(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::I32Add);
    ION_OP_END(f.emitAdd(ValType::I32, MIRType::Int32));
  }

  static uint32_t on_i64_clz(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::I64Clz);
    ION_OP_END(f.emitUnaryWithType<MClz>(ValType::I64, MIRType::Int64));
  }

  static uint32_t on_i64_ctz(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::I64Ctz);
    ION_OP_END(f.emitUnaryWithType<MCtz>(ValType::I64, MIRType::Int64));
  }

  static uint32_t on_i64_popcnt(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::I64Popcnt);
    ION_OP_END(f.emitUnaryWithType<MPopcnt>(ValType::I64, MIRType::Int64));
  }

  static uint32_t on_f32_abs(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::F32Abs);
    ION_OP_END(f.emitUnaryWithType<MAbs>(ValType::F32, MIRType::Float32));
  }

  static uint32_t on_f32_neg(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::F32Neg);
    ION_OP_END(f.emitUnaryWithType<MWasmNeg>(ValType::F32, MIRType::Float32));
  }

  static uint32_t on_f32_ceil(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::F32Ceil);
    ION_OP_END(f.emitUnaryMathBuiltinCall(SASigCeilF));
  }

  static uint32_t on_f32_floor(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::F32Floor);
    ION_OP_END(f.emitUnaryMathBuiltinCall(SASigFloorF));
  }

  static uint32_t on_f32_trunc(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::F32Trunc);
    ION_OP_END(f.emitUnaryMathBuiltinCall(SASigTruncF));
  }

  static uint32_t on_f32_nearest(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::F32Nearest);
    ION_OP_END(f.emitUnaryMathBuiltinCall(SASigNearbyIntF));
  }

  static uint32_t on_f32_sqrt(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::F32Sqrt);
    ION_OP_END(f.emitUnaryWithType<MSqrt>(ValType::F32, MIRType::Float32));
  }

  static uint32_t on_f64_abs(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::F64Abs);
    ION_OP_END(f.emitUnaryWithType<MAbs>(ValType::F64, MIRType::Double));
  }

  static uint32_t on_f64_neg(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::F64Neg);
    ION_OP_END(f.emitUnaryWithType<MWasmNeg>(ValType::F64, MIRType::Double));
  }

  static uint32_t on_f64_ceil(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::F64Ceil);
    ION_OP_END(f.emitUnaryMathBuiltinCall(SASigCeilD));
  }

  static uint32_t on_f64_floor(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::F64Floor);
    ION_OP_END(f.emitUnaryMathBuiltinCall(SASigFloorD));
  }

  static uint32_t on_f64_trunc(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::F64Trunc);
    ION_OP_END(f.emitUnaryMathBuiltinCall(SASigTruncD));
  }

  static uint32_t on_f64_nearest(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::F64Nearest);
    ION_OP_END(f.emitUnaryMathBuiltinCall(SASigNearbyIntD));
  }

  static uint32_t on_f64_sqrt(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::F64Sqrt);
    ION_OP_END(f.emitUnaryWithType<MSqrt>(ValType::F64, MIRType::Double));
  }

  static uint32_t on_i32_wrap_i64(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::I32WrapI64);
    ION_OP_END(f.emitWrapI32());
  }

  static uint32_t on_i32_trunc_f32_s(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::I32TruncF32S);
    ION_OP_END(f.emitTruncate(ValType::F32, ValType::I32, false, false));
  }

  static uint32_t on_i32_trunc_f32_u(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::I32TruncF32U);
    ION_OP_END(f.emitTruncate(ValType::F32, ValType::I32, true, false));
  }

  static uint32_t on_i32_trunc_f64_s(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::I32TruncF64S);
    ION_OP_END(f.emitTruncate(ValType::F64, ValType::I32, false, false));
  }

  static uint32_t on_i32_trunc_f64_u(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::I32TruncF64U);
    ION_OP_END(f.emitTruncate(ValType::F64, ValType::I32, true, false));
  }

  static uint32_t on_i64_extend_i32_s(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::I64ExtendI32S);
    ION_OP_END(f.emitExtendI32(false));
  }

  static uint32_t on_i64_extend_i32_u(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::I64ExtendI32U);
    ION_OP_END(f.emitExtendI32(true));
  }

  static uint32_t on_i64_trunc_f32_s(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::I64TruncF32S);
    ION_OP_END(f.emitTruncate(ValType::F32, ValType::I64, false, false));
  }

  static uint32_t on_i64_trunc_f32_u(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::I64TruncF32U);
    ION_OP_END(f.emitTruncate(ValType::F32, ValType::I64, true, false));
  }

  static uint32_t on_i64_trunc_f64_s(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::I64TruncF64S);
    ION_OP_END(f.emitTruncate(ValType::F64, ValType::I64, false, false));
  }

  static uint32_t on_i64_trunc_f64_u(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::I64TruncF64U);
    ION_OP_END(f.emitTruncate(ValType::F64, ValType::I64, true, false));
  }

  static uint32_t on_f32_convert_i32_s(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::F32ConvertI32S);
    ION_OP_END(f.emitConversion<MToFloat32>(ValType::I32, ValType::F32));
  }

  static uint32_t on_f32_convert_i32_u(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::F32ConvertI32U);
    ION_OP_END(
        f.emitConversion<MWasmUnsignedToFloat32>(ValType::I32, ValType::F32));
  }

  static uint32_t on_f32_convert_i64_s(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::F32ConvertI64S);
    ION_OP_END(
        f.emitConvertI64ToFloatingPoint(ValType::F32, MIRType::Float32, false));
  }

  static uint32_t on_f32_convert_i64_u(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::F32ConvertI64U);
    ION_OP_END(
        f.emitConvertI64ToFloatingPoint(ValType::F32, MIRType::Float32, true));
  }

  static uint32_t on_f32_demote_f64(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::F32DemoteF64);
    ION_OP_END(f.emitConversion<MToFloat32>(ValType::F64, ValType::F32));
  }

  static uint32_t on_f64_convert_i32_s(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::F64ConvertI32S);
    ION_OP_END(f.emitConversion<MToDouble>(ValType::I32, ValType::F64));
  }

  static uint32_t on_f64_convert_i32_u(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::F64ConvertI32U);
    ION_OP_END(
        f.emitConversion<MWasmUnsignedToDouble>(ValType::I32, ValType::F64));
  }

  static uint32_t on_f64_convert_i64_s(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::F64ConvertI64S);
    ION_OP_END(
        f.emitConvertI64ToFloatingPoint(ValType::F64, MIRType::Double, false));
  }

  static uint32_t on_f64_convert_i64_u(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::F64ConvertI64U);
    ION_OP_END(
        f.emitConvertI64ToFloatingPoint(ValType::F64, MIRType::Double, true));
  }

  static uint32_t on_f64_promote_f32(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::F64PromoteF32);
    ION_OP_END(f.emitConversion<MToDouble>(ValType::F32, ValType::F64));
  }

  static uint32_t on_i32_reinterpret_f32(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::I32ReinterpretF32);
    ION_OP_END(f.emitReinterpret(ValType::I32, ValType::F32, MIRType::Int32));
  }

  static uint32_t on_i64_reinterpret_f64(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::I64ReinterpretF64);
    ION_OP_END(f.emitReinterpret(ValType::I64, ValType::F64, MIRType::Int64));
  }

  static uint32_t on_f32_reinterpret_i32(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::F32ReinterpretI32);
    ION_OP_END(f.emitReinterpret(ValType::F32, ValType::I32, MIRType::Float32));
  }

  static uint32_t on_f64_reinterpret_i64(const VwHookCtx* c, const VwState*) {
    ION_OP_BEGIN(c, Op::F64ReinterpretI64);
    ION_OP_END(f.emitReinterpret(ValType::F64, ValType::I64, MIRType::Double));
  }
};

static const VwVisitor& IonItascaVisitor() {
  static const VwVisitor visitor = [] {
    VwVisitor v{};
    v.on_function_start = IonItascaHooks::on_function_start;
#define ION_INSTALL(field) v.field = IonItascaHooks::field;
    ION_INSTALL(on_unreachable)
    ION_INSTALL(on_nop)
    ION_INSTALL(on_block)
    ION_INSTALL(on_loop)
    ION_INSTALL(on_if)
    ION_INSTALL(on_else)
    ION_INSTALL(on_end)
    ION_INSTALL(on_br)
    ION_INSTALL(on_br_if)
    ION_INSTALL(on_br_table_label)
    ION_INSTALL(on_br_table)
    ION_INSTALL(on_return)
    ION_INSTALL(on_call)
    ION_INSTALL(on_call_indirect)
    ION_INSTALL(on_drop)
    ION_INSTALL(on_select)
    ION_INSTALL(on_local_get)
    ION_INSTALL(on_local_set)
    ION_INSTALL(on_local_tee)
    ION_INSTALL(on_global_get)
    ION_INSTALL(on_global_set)
    ION_INSTALL(on_memory_size)
    ION_INSTALL(on_memory_grow)
    ION_INSTALL(on_i32_const)
    ION_INSTALL(on_i64_const)
    ION_INSTALL(on_f32_const)
    ION_INSTALL(on_f64_const)
#undef ION_INSTALL
#define ION_INSTALL_MEM(hook, Name) v.memory.hook = IonItascaHooks::hook;
#define ION_INSTALL_NUMERIC(hook, Name) v.numeric.hook = IonItascaHooks::hook;
    SM_VW_FOR_EACH_MEMORY_OP(ION_INSTALL_MEM)
    SM_VW_FOR_EACH_NUMERIC_OP(ION_INSTALL_NUMERIC)
#undef ION_INSTALL_MEM
#undef ION_INSTALL_NUMERIC
    AssertVisitorIsComplete(v);
    return v;
  }();
  return visitor;
}

#undef ION_OP_BEGIN
#undef ION_OP_END

bool RootCompiler::generate() {
  // Only activate branch hinting if some hints were parsed.
  if (!codeMeta_.branchHints.isEmpty()) {
    compileInfo_.setBranchHinting(true);
  }

  // Figure out what the inlining budget for this function is.  If we've
  // already exceeded the module-level limit, the budget is zero.  See
  // "[SMDOC] Per-function and per-module inlining limits" (WasmHeuristics.h)
  if (codeTailMeta_) {
    auto guard = codeTailMeta_->inliningBudget.lock();

    if (guard.get() > 0) {
      localInliningBudget_ =
          int64_t(codeMeta_.codeSectionSize()) * PerFunctionMaxInliningRatio;
      localInliningBudget_ =
          std::min<int64_t>(localInliningBudget_, guard.get());
    } else {
      localInliningBudget_ = 0;
    }
    MOZ_ASSERT(localInliningBudget_ >= 0);
  } else {
    localInliningBudget_ = 0;
  }

  // Build the MIR graph. itasca owns the loop over the body and reports each
  // accepted operator to the hooks, which are this compiler. Its locals, and
  // so initRoot/startBlock/beginPushedBody, wait for on_function_start.
  FunctionCompiler funcCompiler(*this, decoder_, func_, compileInfo_);
  {
    IonDriver driver{
        funcCompiler,
        codeBytes_.start + (validationBatch_ ? func_.entryOffset : 0)};
    if (!DriveBodyWithItasca(codeBytes_, func_.entryOffset, itascaEnv_,
                             func_.index - codeMeta_.numFuncImports,
                             IonItascaVisitor(), &driver, &driver.decline,
                             error_, validationBatch_)) {
      return false;
    }
  }
  funcCompiler.finish();
  observedFeatures_ = funcCompiler.featureUsage();

  MOZ_ASSERT(loopDepth_ == 0);

  funcStats_.numFuncs += 1;
  funcStats_.bytecodeSize += func_.bytecodeSize();
  funcStats_.inlinedDirectCallCount += inliningStats_.inlinedDirectFunctions;
  funcStats_.inlinedCallRefCount += inliningStats_.inlinedCallRefFunctions;
  funcStats_.inlinedDirectCallBytecodeSize +=
      inliningStats_.inlinedDirectBytecodeSize;
  funcStats_.inlinedCallRefBytecodeSize +=
      inliningStats_.inlinedCallRefBytecodeSize;
  funcStats_.numLargeFunctionBackoffs +=
      inliningStats_.largeFunctionBackoff ? 1 : 0;

  if (codeTailMeta_) {
    auto guard = codeTailMeta_->inliningBudget.lock();
    // Update the module's inlining budget accordingly.  If it is already
    // negative, no more inlining for the module can happen, so there's no
    // point in updating it further.
    if (guard.get() >= 0) {
      guard.get() -= int64_t(inliningStats_.inlinedDirectBytecodeSize);
      guard.get() -= int64_t(inliningStats_.inlinedCallRefBytecodeSize);
      if (guard.get() < 0) {
        JS_LOG(wasmPerf, Info,
               "CM=..%06lx  RC::generate            "
               "Inlining budget for entire module exceeded",
               0xFFFFFF & (unsigned long)uintptr_t(&codeMeta_));
      }
    }
    // If this particular root function overran the function-level
    // limit, note that in the module too.
    if (localInliningBudget_ < 0) {
      funcStats_.numInliningBudgetOverruns += 1;
    }
  }

  return true;
}

CompileInfo* RootCompiler::startInlineCall(
    uint32_t callerFuncIndex, BytecodeOffset callerOffset,
    uint32_t calleeFuncIndex, uint32_t numLocals, size_t inlineeBytecodeSize,
    InliningHeuristics::CallKind callKind) {
  if (callKind == InliningHeuristics::CallKind::Direct) {
    inliningStats_.inlinedDirectBytecodeSize += inlineeBytecodeSize;
    inliningStats_.inlinedDirectFunctions += 1;
  } else {
    MOZ_ASSERT(callKind == InliningHeuristics::CallKind::CallRef);
    inliningStats_.inlinedCallRefBytecodeSize += inlineeBytecodeSize;
    inliningStats_.inlinedCallRefFunctions += 1;
  }

  // Update the inlining budget accordingly.  If it is already negative, no
  // more inlining within this root function can happen, so there's no
  // point in updating it further.
  if (localInliningBudget_ >= 0) {
    localInliningBudget_ -= int64_t(inlineeBytecodeSize);
#ifdef JS_JITSPEW
    if (localInliningBudget_ <= 0) {
      JS_LOG(wasmPerf, Info,
             "CM=..%06lx  RC::startInlineCall     "
             "Inlining budget for fI=%u exceeded",
             0xFFFFFF & (unsigned long)uintptr_t(&codeMeta_), callerFuncIndex);
    }
#endif
  }

  // Add the callers offset to the stack of inlined caller offsets
  if (!inlinedCallerOffsets_.append(callerOffset)) {
    return nullptr;
  }

  // Cache a copy of the current stack of inlined caller offsets that can be
  // shared across all call sites
  InlinedCallerOffsets inlinedCallerOffsets;
  if (!inlinedCallerOffsets.appendAll(inlinedCallerOffsets_)) {
    return nullptr;
  }

  if (!inliningContext_.append(std::move(inlinedCallerOffsets),
                               &inlinedCallerOffsetsIndex_)) {
    return nullptr;
  }

  UniqueCompileInfo compileInfo = MakeUnique<CompileInfo>(numLocals);
  if (!compileInfo || !compileInfos_.append(std::move(compileInfo))) {
    return nullptr;
  }
  return compileInfos_[compileInfos_.length() - 1].get();
}

void RootCompiler::finishInlineCall() { inlinedCallerOffsets_.popBack(); }

bool wasm::IonCompileFunctions(
    const CodeMetadata& codeMeta, const CodeTailMetadata* codeTailMeta,
    const CompilerEnvironment& compilerEnv, LifoAlloc& lifo,
    const FuncCompileInputVector& inputs, CompiledCode* code,
    const BytecodeRegion& codeBytes, const VwEnv* itascaEnv, UniqueChars* error,
    VwBatch* validationBatch) {
  MOZ_ASSERT(compilerEnv.tier() == Tier::Optimized);
  MOZ_ASSERT(compilerEnv.debug() == DebugEnabled::False);
  MOZ_ASSERT_IF(compilerEnv.mode() == CompileMode::LazyTiering, !!codeTailMeta);

  // We should not interact with the GC heap, nor allocate from it when we are
  // compiling wasm code. Ion data structures have some fields for GC objects
  // that we do not use, yet can confuse the static analysis here. Disable it
  // for this function.
  JS::AutoSuppressGCAnalysis nogc;

  TempAllocator alloc(&lifo);
  JitContext jitContext;
  MOZ_ASSERT(IsCompilingWasm());
  WasmMacroAssembler masm(alloc);
#if defined(JS_CODEGEN_ARM64)
  masm.SetStackPointer64(PseudoStackPointer64);
#endif

  // Swap in already-allocated empty vectors to avoid malloc/free.
  MOZ_ASSERT(code->empty());
  if (!code->swap(masm)) {
    return false;
  }

  // Create a description of the stack layout created by GenerateTrapExit().
  RegisterOffsets trapExitLayout;
  size_t trapExitLayoutNumWords;
  GenerateTrapExitRegisterOffsets(&trapExitLayout, &trapExitLayoutNumWords);

  for (const FuncCompileInput& func : inputs) {
    JitSpew(JitSpew_Codegen, "\n");
    JitSpew(JitSpew_Codegen,
            "# ================================"
            "==================================");
    JitSpew(JitSpew_Codegen, "# ==");
    JitSpew(JitSpew_Codegen,
            "# wasm::IonCompileFunctions: starting on function index %d",
            (int)func.index);

#ifdef DEBUG
    // Snapshot the "frontier" of the trapsite vectors so we can determine
    // which ones are added to during compilation of this function.
    mozilla::EnumeratedArray<Trap, uint32_t, size_t(Trap::Limit)>
        trapSitesBefore;
    for (Trap kind : mozilla::MakeEnumeratedRange(Trap::Limit)) {
      trapSitesBefore[kind] = uint32_t(masm.trapSites().length(kind));
    }
#endif

    // Decoder for misc error reporting; nothing reads bytes through it any
    // more, the validator does the reading. The frame, including locals,
    // comes from the validator's own on_function_start hook.
    Decoder d(func.begin, func.end, func.bytecodeOffset, error);

    // Set up for Ion compilation.
    RootCompiler rootCompiler(compilerEnv, codeMeta, codeTailMeta, alloc, func,
                              d, masm.tryNotes(), masm.inliningContext(),
                              codeBytes, itascaEnv, error, validationBatch);
    if (!rootCompiler.generate()) {
      return false;
    }

    // Record observed feature usage
    FeatureUsage observedFeatures = rootCompiler.observedFeatures();
    code->featureUsage |= observedFeatures;

    // Compile MIR graph
    {
      rootCompiler.mirGen().spewBeginWasmFunction(func.index);
      jit::AutoSpewEndFunction spewEndFunction(&rootCompiler.mirGen());

      if (!OptimizeMIR(&rootCompiler.mirGen())) {
        return false;
      }

      LIRGraph* lir = GenerateLIR(&rootCompiler.mirGen());
      if (!lir) {
        return false;
      }

      size_t unwindInfoBefore = masm.codeRangeUnwindInfos().length();

      CodeGenerator codegen(&rootCompiler.mirGen(), lir, &masm, &codeMeta);

      TrapSiteDesc prologueTrapSiteDesc(
          wasm::BytecodeOffset(func.bytecodeOffset));
      FuncOffsets offsets;
      ArgTypeVector args(codeMeta.getFuncType(func.index));
      if (!codegen.generateWasm(CallIndirectId::forFunc(codeMeta, func.index),
                                prologueTrapSiteDesc, args, trapExitLayout,
                                trapExitLayoutNumWords, &offsets,
                                &code->stackMaps, &d)) {
        return false;
      }

      bool hasUnwindInfo =
          unwindInfoBefore != masm.codeRangeUnwindInfos().length();

      // Record this function's code range
      if (!code->codeRanges.emplaceBack(func.index, offsets, hasUnwindInfo)) {
        return false;
      }
    }

    if (PerfEnabled()) {
      IonPerfSpewer spewer = std::move(rootCompiler.mirGen().perfSpewer());
      if (!code->funcIonSpewers.emplaceBack(func.index, std::move(spewer))) {
        return false;
      }
    }

    // Record this function's compilation stats
    code->compileStats.merge(rootCompiler.funcStats());

    // Record this function's specific feature usage
    if (!code->funcs.emplaceBack(func.index, observedFeatures)) {
      return false;
    }

#ifdef DEBUG
    // Get a second snapshot of the frontier of the TrapSite vectors, and
    // use this to check that traps that need a stackmap, actually have one.
    mozilla::EnumeratedArray<Trap, uint32_t, size_t(Trap::Limit)>
        trapSitesAfter;
    for (Trap kind : mozilla::MakeEnumeratedRange(Trap::Limit)) {
      trapSitesAfter[kind] = uint32_t(masm.trapSites().length(kind));
    }

    // Do the check.  This asserts if the check fails.
    auto checkThisTrapKind = [](Trap t) -> bool {
      // Temporary setting, to make all of this a no-op.
      return false;
    };
    CheckStackMapsForTraps(masm, code->stackMaps, trapSitesBefore,
                           trapSitesAfter, checkThisTrapKind);
#endif

    JitSpew(JitSpew_Codegen,
            "# wasm::IonCompileFunctions: completed function index %d",
            (int)func.index);
    JitSpew(JitSpew_Codegen, "# ==");
    JitSpew(JitSpew_Codegen,
            "# ================================"
            "==================================");
    JitSpew(JitSpew_Codegen, "\n");
  }

  masm.finish();
  if (masm.oom()) {
    return false;
  }

  return code->swap(masm);
}

bool wasm::IonDumpFunction(const CompilerEnvironment& compilerEnv,
                           const CodeMetadata& codeMeta,
                           const FuncCompileInput& func, GenericPrinter& out,
                           UniqueChars* error) {
  // wasmDumpIon is disabled on this branch; see DumpIonFunctionInModule.
  *error = DuplicateString("wasmDumpIon is disabled on the itasca branch");
  return false;
}

bool js::wasm::IonPlatformSupport() {
#if defined(JS_CODEGEN_X64) || defined(JS_CODEGEN_X86) ||       \
    defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_MIPS64) ||    \
    defined(JS_CODEGEN_ARM64) || defined(JS_CODEGEN_LOONG64) || \
    defined(JS_CODEGEN_RISCV64)
  return true;
#else
  return false;
#endif
}
