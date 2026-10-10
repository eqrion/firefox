/*
 * Copyright 2016 Mozilla Foundation
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

/*
 * [SMDOC] WebAssembly baseline compiler (RabaldrMonkey)
 *
 * For now, see WasmBCClass.h for general comments about the compiler's
 * structure.
 *
 * ----------------
 *
 * General assumptions for 32-bit vs 64-bit code:
 *
 * - A 32-bit register can be extended in-place to a 64-bit register on 64-bit
 *   systems.
 *
 * - Code that knows that Register64 has a '.reg' member on 64-bit systems and
 *   '.high' and '.low' members on 32-bit systems, or knows the implications
 *   thereof, is #ifdef JS_PUNBOX64.  All other code is #if(n)?def JS_64BIT.
 *
 * Coding standards are a little fluid:
 *
 * - In "small" code generating functions (eg emitMultiplyF64, emitQuotientI32,
 *   and surrounding functions; most functions fall into this class) where the
 *   meaning is obvious:
 *
 *   Old school:
 *   - if there is a single source + destination register, it is called 'r'
 *   - if there is one source and a different destination, they are called 'rs'
 *     and 'rd'
 *   - if there is one source + destination register and another source register
 *     they are called 'r' and 'rs'
 *   - if there are two source registers and a destination register they are
 *     called 'rs0', 'rs1', and 'rd'.
 *
 *   The new thing:
 *   - what is called 'r' in the old-school naming scheme is increasingly called
 *     'rsd' in source+dest cases.
 *
 * - Generic temp registers are named /temp[0-9]?/ not /tmp[0-9]?/.
 *
 * - Registers can be named non-generically for their function ('rp' for the
 *   'pointer' register and 'rv' for the 'value' register are typical) and those
 *   names may or may not have an 'r' prefix.
 *
 * - "Larger" code generating functions make their own rules.
 */

/*
 * [SMDOC] WebAssembly baseline compiler -- Lazy Tier-Up mechanism
 *
 * For baseline functions, we compile in code to monitor the function's
 * "hotness" and request tier-up once that hotness crosses a threshold.
 *
 * (1) Each function has an associated int32_t counter,
 *     FuncDefInstanceData::hotnessCounter.  These are stored in an array in
 *     the Instance.  Hence access to them is fast and thread-local.
 *
 * (2) On instantiation, the counters are set to some positive number
 *     (Instance::init, Instance::computeInitialHotnessCounter), which is a
 *     very crude estimate of the cost of Ion compilation of the function.
 *
 * (3) In baseline compilation, a function decrements its counter at every
 *     entry (BaseCompiler::beginFunction) and at the start of every loop
 *     iteration (BaseCompiler::emitLoop).  The decrement code is created by
 *     BaseCompiler::addHotnessCheck.
 *
 * (4) The decrement is by some value in the range 1 .. 127, as computed from
 *     the function or loop-body size, by BlockSizeToDownwardsStep.
 *
 * (5) For loops, the body size is known only at the end of the loop, but the
 *     check is required at the start of the body.  Hence the value is patched
 *     in at the end (BaseCompiler::emitEnd, case LabelKind::Loop).
 *
 * (6) BaseCompiler::addHotnessCheck creates the shortest possible
 *     decrement/check code, to minimise both time and code-space overhead.  On
 *     Intel it is only two instructions.  The counter has the value from (4)
 *     subtracted from it.  If the result is negative, we jump to OOL code
 *     (class OutOfLineRequestTierUp) which requests tier up; control then
 *     continues immediately after the check.
 *
 * (7) The OOL tier-up request code calls the stub pointed to by
 *     Instance::requestTierUpStub_.  This always points to the stub created by
 *     GenerateRequestTierUpStub.  This saves all registers and calls onwards
 *     to WasmHandleRequestTierUp in C++-land.
 *
 * (8) WasmHandleRequestTierUp figures out which function in which Instance is
 *     requesting tier-up.  It sets the function's counter (1) to the largest
 *     possible value, which is 2^31-1.  It then calls onwards to
 *     Code::requestTierUp, which requests off-thread Ion compilation of the
 *     function, then immediately returns.
 *
 * (9) It is important that (8) sets the counter to 2^31-1 (as close to
 *     infinity as possible).  This is because it may be arbitrarily long
 *     before the optimised code becomes available.  In the meantime the
 *     baseline version of the function will continue to run.  We do not want
 *     it to make frequent duplicate requests for tier-up.  Although a request
 *     for tier-up is relatively cheap (a few hundred instructions), it is
 *     still way more expensive than the fast-case for a hotness check (2 insns
 *     on Intel), and performance of the baseline code will be badly affected
 *     if it makes many duplicate requests.
 *
 * (10) Of course it is impossible to *guarantee* that a baseline function will
 *      not make a duplicate request, because the Ion compilation of the
 *      function could take arbitrarily long, or even fail completely (eg OOM).
 *      Hence it is necessary for WasmCode::requestTierUp (8) to detect and
 *      ignore duplicate requests.
 *
 * (11) Each Instance of a Module runs in its own thread and has its own array
 *      of counters.  This makes the counter updating thread-local and cheap.
 *      But it means that, if a Module has multiple threads (Instances), it
 *      could be that a function never gets hot enough to request tier up,
 *      because it is not hot enough in any single thread, even though the
 *      total hotness summed across all threads is enough to request tier up.
 *      Whether this inaccuracy is a problem in practice remains to be seen.
 *
 * (12) Code::requestTierUp (8) creates a PartialTier2CompileTask and queues it
 *      for execution.  It does not do the compilation itself.
 *
 * (13) A PartialTier2CompileTask's runHelperThreadTask (running on a helper
 *      thread) calls CompilePartialTier2.  This compiles the function with Ion
 *      and racily updates the tiering table entry for the function, which
 *      lives in Code::jumpTables_::tiering_.
 *
 * (14) Subsequent calls to the function's baseline entry points will then jump
 *      to the Ion version of the function.  Hence lazy tier-up is achieved.
 */

#include "wasm/WasmBaselineCompile.h"

#include "wasm/WasmAnyRef.h"
#include "wasm/WasmBCClass.h"
#include "wasm/WasmBCDefs.h"
#include "wasm/WasmBCFrame.h"
#include "wasm/WasmBCRegDefs.h"
#include "wasm/WasmBCStk.h"
#include "wasm/WasmGC.h"
#include "wasm/WasmValType.h"
#include "wasm/WasmItasca.h"
#include "wasm/WasmItascaOps.h"

#include "jit/MacroAssembler-inl.h"
#include "wasm/WasmBCClass-inl.h"
#include "wasm/WasmBCCodegen-inl.h"
#include "wasm/WasmBCRegDefs-inl.h"
#include "wasm/WasmBCRegMgmt-inl.h"
#include "wasm/WasmBCStkMgmt-inl.h"

namespace js {
namespace wasm {

using namespace js::jit;

using mozilla::Maybe;
using mozilla::Nothing;
using mozilla::Some;

////////////////////////////////////////////////////////////
//
// Out of line code management.

// The baseline compiler will use OOL code more sparingly than Ion since our
// code is not high performance and frills like code density and branch
// prediction friendliness will be less important.
class OutOfLineCode : public TempObject {
 private:
  NonAssertingLabel entry_;
  NonAssertingLabel rejoin_;
  StackHeight stackHeight_;

 public:
  OutOfLineCode() : stackHeight_(StackHeight::Invalid()) {}

  Label* entry() { return &entry_; }
  Label* rejoin() { return &rejoin_; }

  void setStackHeight(StackHeight stackHeight) {
    MOZ_ASSERT(!stackHeight_.isValid());
    stackHeight_ = stackHeight;
  }

  void bind(BaseStackFrame* fr, MacroAssembler* masm) {
    MOZ_ASSERT(stackHeight_.isValid());
    masm->bind(&entry_);
    fr->setStackHeight(stackHeight_);
  }

  // The generate() method must be careful about register use because it will be
  // invoked when there is a register assignment in the BaseCompiler that does
  // not correspond to the available registers when the generated OOL code is
  // executed.  The register allocator *must not* be called.
  //
  // The best strategy is for the creator of the OOL object to allocate all
  // temps that the OOL code will need.
  //
  // Input, output, and temp registers are embedded in the OOL object and are
  // known to the code generator.
  //
  // Scratch registers are available to use in OOL code.
  //
  // All other registers must be explicitly saved and restored by the OOL code
  // before being used.

  virtual void generate(MacroAssembler* masm, BaseCompiler* bc) = 0;
};

class OutOfLineTrap : public OutOfLineCode {
  Trap trap_;
  TrapSiteDesc desc_;
  wasm::StackMap* stackMap_;

 public:
  OutOfLineTrap(Trap trap, const TrapSiteDesc& desc, wasm::StackMap* stackMap)
      : trap_(trap), desc_(desc), stackMap_(stackMap) {}

  virtual void generate(MacroAssembler* masm, BaseCompiler* bc) override {
    bc->trap(trap_, desc_, stackMap_);
    if (rejoin()) {
      masm->jump(rejoin());
    }
  }
};

OutOfLineCode* BaseCompiler::addOutOfLineCode(OutOfLineCode* ool) {
  if (!ool || !outOfLine_.append(ool)) {
    return nullptr;
  }
  ool->setStackHeight(fr.stackHeight());
  return ool;
}

bool BaseCompiler::generateOutOfLineCode() {
  for (auto* ool : outOfLine_) {
    if (!ool->entry()->used()) {
      continue;
    }
    ool->bind(&fr, &masm);
    ool->generate(&masm, this);
  }

  return !masm.oom();
}

//////////////////////////////////////////////////////////////////////////////
//
// Sundry code generation.

bool BaseCompiler::addInterruptCheck() {
#ifdef RABALDR_PIN_INSTANCE
  Register tmp(InstanceReg);
#else
  ScratchI32 tmp(*this);
  fr.loadInstancePtr(tmp);
#endif
  Label ok;
  masm.branch32(Assembler::Equal,
                Address(tmp, wasm::Instance::offsetOfInterrupt()), Imm32(0),
                &ok);
  trap(wasm::Trap::CheckInterrupt);
  masm.bind(&ok);
  // stackmap for: interrupt check
  return createStackMap(Some(wasm::Trap::CheckInterrupt));
}

void BaseCompiler::checkDivideByZero(RegI32 rhs) {
  Label nonZero;
  masm.branchTest32(Assembler::NonZero, rhs, rhs, &nonZero);
  trap(Trap::IntegerDivideByZero);
  masm.bind(&nonZero);
}

void BaseCompiler::checkDivideByZero(RegI64 r) {
  Label nonZero;
  ScratchI32 scratch(*this);
  masm.branchTest64(Assembler::NonZero, r, r, scratch, &nonZero);
  trap(Trap::IntegerDivideByZero);
  masm.bind(&nonZero);
}

void BaseCompiler::checkDivideSignedOverflow(RegI32 rhs, RegI32 srcDest,
                                             Label* done, bool zeroOnOverflow) {
  Label notMin;
  masm.branch32(Assembler::NotEqual, srcDest, Imm32(INT32_MIN), &notMin);
  if (zeroOnOverflow) {
    masm.branch32(Assembler::NotEqual, rhs, Imm32(-1), &notMin);
    moveImm32(0, srcDest);
    masm.jump(done);
  } else {
    masm.branch32(Assembler::NotEqual, rhs, Imm32(-1), &notMin);
    trap(Trap::IntegerOverflow);
  }
  masm.bind(&notMin);
}

void BaseCompiler::checkDivideSignedOverflow(RegI64 rhs, RegI64 srcDest,
                                             Label* done, bool zeroOnOverflow) {
  Label notmin;
  masm.branch64(Assembler::NotEqual, srcDest, Imm64(INT64_MIN), &notmin);
  masm.branch64(Assembler::NotEqual, rhs, Imm64(-1), &notmin);
  if (zeroOnOverflow) {
    masm.xor64(srcDest, srcDest);
    masm.jump(done);
  } else {
    trap(Trap::IntegerOverflow);
  }
  masm.bind(&notmin);
}

void BaseCompiler::jumpTable(const LabelVector& labels, Label* theTable) {
  // Flush constant pools to ensure that the table is never interrupted by
  // constant pool entries.
  masm.flush();

#if defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_ARM64) || \
    defined(JS_CODEGEN_RISCV64)
  // Prevent nop sequences to appear in the jump table.
  AutoForbidNops afn(&masm);
#endif
  masm.bind(theTable);

  for (const auto& label : labels) {
    CodeLabel cl;
    masm.writeCodePointer(&cl);
    cl.target()->bind(label.offset());
    masm.addCodeLabel(cl);
  }
}

void BaseCompiler::tableSwitch(Label* theTable, RegI32 switchValue,
                               Label* dispatchCode) {
  masm.bind(dispatchCode);

#if defined(JS_CODEGEN_X64) || defined(JS_CODEGEN_X86)
  ScratchI32 scratch(*this);
  CodeLabel tableCl;

  masm.mov(&tableCl, scratch);

  tableCl.target()->bind(theTable->offset());
  masm.addCodeLabel(tableCl);

  masm.jmp(Operand(scratch, switchValue, ScalePointer));
#elif defined(JS_CODEGEN_ARM)
  // Flush constant pools: offset must reflect the distance from the MOV
  // to the start of the table; as the address of the MOV is given by the
  // label, nothing must come between the bind() and the ma_mov().
  AutoForbidPoolsAndNops afp(&masm,
                             /* number of instructions in scope = */ 5);

  ScratchI32 scratch(*this);

  // Compute the offset from the ma_mov instruction to the jump table.
  Label here;
  masm.bind(&here);
  uint32_t offset = here.offset() - theTable->offset();

  // Read PC+8
  masm.ma_mov(pc, scratch);

  // ARM scratch register is required by ma_sub.
  ScratchRegisterScope arm_scratch(*this);

  // Compute the absolute table base pointer into `scratch`, offset by 8
  // to account for the fact that ma_mov read PC+8.
  masm.ma_sub(Imm32(offset + 8), scratch, arm_scratch);

  // Jump indirect via table element.
  masm.ma_ldr(DTRAddr(scratch, DtrRegImmShift(switchValue, LSL, 2)), pc, Offset,
              Assembler::Always);
#elif defined(JS_CODEGEN_MIPS64) || defined(JS_CODEGEN_LOONG64) || \
    defined(JS_CODEGEN_RISCV64)
  ScratchI32 scratch(*this);
  CodeLabel tableCl;

  masm.ma_li(scratch, &tableCl);

  tableCl.target()->bind(theTable->offset());
  masm.addCodeLabel(tableCl);

  masm.branchToComputedAddress(BaseIndex(scratch, switchValue, ScalePointer));
#elif defined(JS_CODEGEN_ARM64)
  AutoForbidPoolsAndNops afp(&masm,
                             /* number of instructions in scope = */ 4);

  ScratchI32 scratch(*this);

  ARMRegister s(scratch, 64);
  ARMRegister v(switchValue, 64);
  masm.Adr(s, theTable);
  masm.Add(s, s, Operand(v, vixl::LSL, 3));
  masm.Ldr(s, MemOperand(s, 0));
  masm.Br(s);
#else
  MOZ_CRASH("BaseCompiler platform hook: tableSwitch");
#endif
}

// Helpers for accessing the "baseline scratch" areas: all targets
void BaseCompiler::stashWord(RegPtr instancePtr, size_t index, RegPtr r) {
  MOZ_ASSERT(r != instancePtr);
  MOZ_ASSERT(index < Instance::N_BASELINE_SCRATCH_WORDS);
  masm.storePtr(r,
                Address(instancePtr, Instance::offsetOfBaselineScratchWords() +
                                         index * sizeof(uintptr_t)));
}

void BaseCompiler::unstashWord(RegPtr instancePtr, size_t index, RegPtr r) {
  MOZ_ASSERT(index < Instance::N_BASELINE_SCRATCH_WORDS);
  masm.loadPtr(Address(instancePtr, Instance::offsetOfBaselineScratchWords() +
                                        index * sizeof(uintptr_t)),
               r);
}

// Helpers for accessing the "baseline scratch" areas: X86 only
#ifdef JS_CODEGEN_X86
void BaseCompiler::stashI64(RegPtr regForInstance, RegI64 r) {
  static_assert(sizeof(uintptr_t) == 4);
  MOZ_ASSERT(Instance::sizeOfBaselineScratchWords() >= 8);
  MOZ_ASSERT(regForInstance != r.low && regForInstance != r.high);
#  ifdef RABALDR_PIN_INSTANCE
#    error "Pinned instance not expected"
#  endif
  fr.loadInstancePtr(regForInstance);
  masm.store32(
      r.low, Address(regForInstance, Instance::offsetOfBaselineScratchWords()));
  masm.store32(r.high, Address(regForInstance,
                               Instance::offsetOfBaselineScratchWords() + 4));
}

void BaseCompiler::unstashI64(RegPtr regForInstance, RegI64 r) {
  static_assert(sizeof(uintptr_t) == 4);
  MOZ_ASSERT(Instance::sizeOfBaselineScratchWords() >= 8);
#  ifdef RABALDR_PIN_INSTANCE
#    error "Pinned instance not expected"
#  endif
  fr.loadInstancePtr(regForInstance);
  if (regForInstance == r.low) {
    masm.load32(
        Address(regForInstance, Instance::offsetOfBaselineScratchWords() + 4),
        r.high);
    masm.load32(
        Address(regForInstance, Instance::offsetOfBaselineScratchWords()),
        r.low);
  } else {
    masm.load32(
        Address(regForInstance, Instance::offsetOfBaselineScratchWords()),
        r.low);
    masm.load32(
        Address(regForInstance, Instance::offsetOfBaselineScratchWords() + 4),
        r.high);
  }
}
#endif

// Given the bytecode size of a block (a complete function body, or a loop
// body), return the required downwards step for the associated hotness
// counter.  Returned value will be in 1 .. 127 inclusive.
static uint32_t BlockSizeToDownwardsStep(size_t blockBytecodeSize) {
  MOZ_RELEASE_ASSERT(blockBytecodeSize <= size_t(MaxFunctionBytes));
  const uint32_t BYTECODES_PER_STEP = 20;  // tunable parameter
  size_t step = blockBytecodeSize / BYTECODES_PER_STEP;
  step = std::max<uint32_t>(step, 1);
  step = std::min<uint32_t>(step, 127);
  return uint32_t(step);
}

//////////////////////////////////////////////////////////////////////////////
//
// Function entry and exit

bool BaseCompiler::beginFunction() {
  AutoCreatedBy acb(masm, "(wasm)BaseCompiler::beginFunction");

  JitSpew(JitSpew_Codegen, "# ================================");
  JitSpew(JitSpew_Codegen, "# Emitting wasm baseline code");
  JitSpew(JitSpew_Codegen,
          "# beginFunction: start of function prologue for index %d",
          (int)func_.index);

  // Make a start on the stackmap for this function.  Inspect the args so
  // as to determine which of them are both in-memory and pointer-typed, and
  // add entries to machineStackTracker as appropriate.

  ArgTypeVector args(funcType());
  size_t inboundStackArgBytes = StackArgAreaSizeUnaligned(args, ABIKind::Wasm);
  MOZ_ASSERT(inboundStackArgBytes % sizeof(void*) == 0);
  stackMapGenerator_.numStackArgBytes = inboundStackArgBytes;

  MOZ_ASSERT(stackMapGenerator_.machineStackTracker.length() == 0);
  if (!stackMapGenerator_.machineStackTracker.pushNonGCPointers(
          stackMapGenerator_.numStackArgBytes / sizeof(void*))) {
    return false;
  }

  // Identify GC-managed pointers passed on the stack.
  for (ABIArgIter i(args, ABIKind::Wasm); !i.done(); i++) {
    ABIArg argLoc = *i;
    if (argLoc.kind() == ABIArg::Stack &&
        args[i.index()] == MIRType::WasmAnyRef) {
      uint32_t offset = argLoc.offsetFromArgBase();
      MOZ_ASSERT(offset < inboundStackArgBytes);
      MOZ_ASSERT(offset % sizeof(void*) == 0);
      stackMapGenerator_.machineStackTracker.setGCPointer(offset /
                                                          sizeof(void*));
    }
  }

  perfSpewer_.startRecording();
  perfSpewer_.markStartOffset(masm.currentOffset());
  perfSpewer_.recordOffset(masm, "Prologue");
  GenerateFunctionPrologue(
      masm, CallIndirectId::forFunc(codeMeta_, func_.index),
      compilerEnv_.mode() != CompileMode::Once ? Some(func_.index) : Nothing(),
      &offsets_);

  // GenerateFunctionPrologue pushes exactly one wasm::Frame's worth of
  // stuff, and none of the values are GC pointers.  Hence:
  if (!stackMapGenerator_.machineStackTracker.pushNonGCPointers(
          sizeof(Frame) / sizeof(void*))) {
    return false;
  }

  // Initialize DebugFrame fields before the stack overflow trap so that
  // we have the invariant that all observable Frames in a debugEnabled
  // Module have valid DebugFrames.
  if (compilerEnv_.debugEnabled()) {
#ifdef JS_CODEGEN_ARM64
    static_assert(DebugFrame::offsetOfFrame() % WasmStackAlignment == 0,
                  "aligned");
#endif
    masm.reserveStack(DebugFrame::offsetOfFrame());
    if (!stackMapGenerator_.machineStackTracker.pushNonGCPointers(
            DebugFrame::offsetOfFrame() / sizeof(void*))) {
      return false;
    }

    masm.store32(Imm32(func_.index), Address(masm.getStackPointer(),
                                             DebugFrame::offsetOfFuncIndex()));
    masm.store32(Imm32(0),
                 Address(masm.getStackPointer(), DebugFrame::offsetOfFlags()));

    // No need to initialize cachedReturnJSValue_ or any ref-typed spilled
    // register results, as they are traced if and only if a corresponding
    // flag (hasCachedReturnJSValue or hasSpilledRefRegisterResult) is set.
  }

  // Generate a stack-overflow check and its associated stackmap.

  ExitStubMapVector extras;
  StackMap* functionEntryStackMap;
  if (!stackMapGenerator_.generateStackmapEntriesForTrapExit(args, &extras) ||
      // stackmap for: stack overflow check
      !stackMapGenerator_.createStackMap(Some(Trap::StackOverflow), extras,
                                         HasDebugFrameWithLiveRefs::No, stk_,
                                         &functionEntryStackMap)) {
    return false;
  }

  OutOfLineCode* oolStackOverflowTrap =
      addOutOfLineCode(new (alloc_) OutOfLineTrap(
          Trap::StackOverflow,
          TrapSiteDesc(BytecodeOffset(func_.bytecodeOffset)), nullptr));
  if (!oolStackOverflowTrap) {
    return false;
  }
  fr.checkStack(ABINonArgReg0, ABINonArgReg1, oolStackOverflowTrap->entry());

  OutOfLineCode* oolInterruptTrap = addOutOfLineCode(new (alloc_) OutOfLineTrap(
      Trap::CheckInterrupt, trapSiteDesc(), functionEntryStackMap));
  if (!oolInterruptTrap) {
    return false;
  }
  masm.branch32(Assembler::NotEqual,
                Address(InstanceReg, wasm::Instance::offsetOfInterrupt()),
                Imm32(0), oolInterruptTrap->entry());
  masm.bind(oolInterruptTrap->rejoin());

  size_t reservedBytes = fr.fixedAllocSize() - masm.framePushed();
  MOZ_ASSERT(0 == (reservedBytes % sizeof(void*)));

  masm.reserveStack(reservedBytes);
  fr.onFixedStackAllocated();
  if (!stackMapGenerator_.machineStackTracker.pushNonGCPointers(
          reservedBytes / sizeof(void*))) {
    return false;
  }

  // Locals are stack allocated.  Mark ref-typed ones in the stackmap
  // accordingly.
  for (const Local& l : localInfo_) {
    // Locals that are stack arguments were already added to the stackmap
    // before pushing the frame.
    if (l.type == MIRType::WasmAnyRef && !l.isStackArgument()) {
      uint32_t offs = fr.localOffsetFromSp(l);
      MOZ_ASSERT(0 == (offs % sizeof(void*)));
      stackMapGenerator_.machineStackTracker.setGCPointer(offs / sizeof(void*));
    }
  }

  // Copy arguments from registers to stack.
  for (ABIArgIter i(args, ABIKind::Wasm); !i.done(); i++) {
    if (args.isSyntheticStackResultPointerArg(i.index())) {
      // If there are stack results and the pointer to stack results
      // was passed in a register, store it to the stack.
      if (i->argInRegister()) {
        fr.storeIncomingStackResultAreaPtr(RegPtr(i->gpr()));
      }
      // If we're in a debug frame, copy the stack result pointer arg
      // to a well-known place.
      if (compilerEnv_.debugEnabled()) {
        Register target = ABINonArgReturnReg0;
        fr.loadIncomingStackResultAreaPtr(RegPtr(target));
        size_t debugFrameOffset =
            masm.framePushed() - DebugFrame::offsetOfFrame();
        size_t debugStackResultsPointerOffset =
            debugFrameOffset + DebugFrame::offsetOfStackResultsPointer();
        masm.storePtr(target, Address(masm.getStackPointer(),
                                      debugStackResultsPointerOffset));
      }
      continue;
    }
    if (!i->argInRegister()) {
      continue;
    }
    Local& l = localInfo_[args.naturalIndex(i.index())];
    switch (i.mirType()) {
      case MIRType::Int32:
        fr.storeLocalI32(RegI32(i->gpr()), l);
        break;
      case MIRType::Int64:
        fr.storeLocalI64(RegI64(i->gpr64()), l);
        break;
      case MIRType::WasmAnyRef: {
        mozilla::DebugOnly<uint32_t> offs = fr.localOffsetFromSp(l);
        MOZ_ASSERT(0 == (offs % sizeof(void*)));
        fr.storeLocalRef(RegRef(i->gpr()), l);
        // We should have just visited this local in the preceding loop.
        MOZ_ASSERT(stackMapGenerator_.machineStackTracker.isGCPointer(
            offs / sizeof(void*)));
        break;
      }
      case MIRType::Double:
        fr.storeLocalF64(RegF64(i->fpu()), l);
        break;
      case MIRType::Float32:
        fr.storeLocalF32(RegF32(i->fpu()), l);
        break;
#ifdef ENABLE_WASM_SIMD
      case MIRType::Simd128:
        fr.storeLocalV128(RegV128(i->fpu()), l);
        break;
#endif
      default:
        MOZ_CRASH("Function argument type");
    }
  }

  fr.zeroLocals(&ra);
  fr.storeInstancePtr(InstanceReg);

  if (compilerEnv_.debugEnabled()) {
    insertBreakablePoint(CallSiteKind::EnterFrame);
    // stackmap for: debug: enter-frame breakpoint
    if (!createStackMap(Nothing() /* stackmap pertains to a call */)) {
      return false;
    }
  }

  JitSpew(JitSpew_Codegen,
          "# beginFunction: enter body with masm.framePushed = %u",
          masm.framePushed());
  MOZ_ASSERT(stackMapGenerator_.framePushedAtEntryToBody.isNothing());
  stackMapGenerator_.framePushedAtEntryToBody.emplace(masm.framePushed());

  if (compilerEnv_.mode() == CompileMode::LazyTiering) {
    size_t funcBytecodeSize = func_.end - func_.begin;
    uint32_t step = BlockSizeToDownwardsStep(funcBytecodeSize);

    // Create a patchable hotness check and patch it immediately (only because
    // there's no way to directly create a non-patchable check directly).
    Maybe<CodeOffset> ctrDecOffset = addHotnessCheck();
    if (ctrDecOffset.isNothing()) {
      return false;
    }
    patchHotnessCheck(ctrDecOffset.value(), step);
  }

  return true;
}

bool BaseCompiler::endFunction() {
  AutoCreatedBy acb(masm, "(wasm)BaseCompiler::endFunction");

  JitSpew(JitSpew_Codegen, "# endFunction: start of function epilogue");

  // Always branch to returnLabel_.
  masm.breakpoint();

  // Patch the add in the prologue so that it checks against the correct
  // frame size. Flush the constant pool in case it needs to be patched.
  masm.flush();

  // Precondition for patching.
  if (masm.oom()) {
    return false;
  }

  fr.patchCheckStack();

  // We could skip generating the epilogue for functions which never return,
  // but that would mess up invariants that all functions have a return address
  // offset in CodeRange. It's also a rare thing, so not worth optimizing for.
  deadCode_ = !returnLabel_.used();
  masm.bind(&returnLabel_);

  ResultType resultType(ResultType::Vector(funcType().results()));

  popStackReturnValues(resultType);

  if (compilerEnv_.debugEnabled() && !deadCode_) {
    // Store and reload the return value from DebugFrame::return so that
    // it can be clobbered, and/or modified by the debug trap.
    saveRegisterReturnValues(resultType);
    insertBreakablePoint(CallSiteKind::Breakpoint);
    // stackmap for: debug: return-point breakpoint
    if (!createStackMap(Nothing() /* stackmap pertains to a call */,
                        HasDebugFrameWithLiveRefs::Maybe)) {
      return false;
    }

    insertBreakablePoint(CallSiteKind::LeaveFrame);
    // stackmap for: debug: leave-frame breakpoint
    if (!createStackMap(Nothing() /* stackmap pertains to a call */,
                        HasDebugFrameWithLiveRefs::Maybe)) {
      return false;
    }

    restoreRegisterReturnValues(resultType);
  }

#ifndef RABALDR_PIN_INSTANCE
  // To satisy instance extent invariant we need to reload InstanceReg because
  // baseline can clobber it.
  fr.loadInstancePtr(InstanceReg);
#endif
  perfSpewer_.recordOffset(masm, "Epilogue");
  GenerateFunctionEpilogue(masm, fr.fixedAllocSize(), &offsets_);

#if defined(JS_ION_PERF)
  // FIXME - profiling code missing.  No bug for this.

  // Note the end of the inline code and start of the OOL code.
  // gen->perfSpewer().noteEndInlineCode(masm);
#endif
  JitSpew(JitSpew_Codegen, "# endFunction: end of function epilogue");

  JitSpew(JitSpew_Codegen, "# endFunction: start of OOL code");
  perfSpewer_.recordOffset(masm, "OOLCode");
  if (!generateOutOfLineCode()) {
    return false;
  }
  JitSpew(JitSpew_Codegen, "# endFunction: end of OOL code");

  if (compilerEnv_.debugEnabled()) {
    JitSpew(JitSpew_Codegen, "# endFunction: start of per-function debug stub");
    insertPerFunctionDebugStub();
    JitSpew(JitSpew_Codegen, "# endFunction: end of per-function debug stub");
  }

  offsets_.end = masm.currentOffset();

  if (!fr.checkStackHeight()) {
    return decoder_.fail(decoder_.beginOffset(), "stack frame is too large");
  }

  perfSpewer_.endRecording();

  JitSpew(JitSpew_Codegen, "# endFunction: end of OOL code for index %d",
          (int)func_.index);
  return !masm.oom();
}

//////////////////////////////////////////////////////////////////////////////
//
// Debugger API.

// [SMDOC] Wasm debug traps -- code details
//
// There are four pieces of code involved.
//
// (1) The "breakable point".  This is placed at every location where we might
//     want to transfer control to the debugger, most commonly before every
//     bytecode.  It must be as short and fast as possible.  It checks
//     Instance::debugStub_, which is either null or a pointer to (3).  If
//     non-null, a call to (2) is performed; when null, nothing happens.
//
// (2) The "per function debug stub".  There is one per function.  It consults
//     a bit-vector attached to the Instance, to see whether breakpoints for
//     the current function are enabled.  If not, it returns (to (1), hence
//     having no effect).  Otherwise, it jumps (not calls) onwards to (3).
//
// (3) The "debug stub" -- not to be confused with the "per function debug
//     stub".  There is one per module.  This saves all the registers and
//     calls onwards to (4), which is in C++ land.  When that call returns,
//     (3) itself returns, which transfers control directly back to (after)
//     (1).
//
// (4) In C++ land -- WasmHandleDebugTrap, corresponding to
//     SymbolicAddress::HandleDebugTrap.  This contains the detailed logic
//     needed to handle the breakpoint.

void BaseCompiler::insertBreakablePoint(CallSiteKind kind) {
  MOZ_ASSERT(!deadCode_);

  // A sync() must happen before this. The debug stub does not save all live
  // registers.
  MOZ_ASSERT(!hasLiveRegsOnStk());

#ifndef RABALDR_PIN_INSTANCE
  fr.loadInstancePtr(InstanceReg);
#endif

  // The breakpoint code must call the breakpoint handler installed on the
  // instance if it is not null.  There is one breakable point before
  // every bytecode, and one at the beginning and at the end of the function.
  //
  // There are many constraints:
  //
  //  - Code should be read-only; we do not want to patch
  //  - The breakpoint code should be as dense as possible, given the volume of
  //    breakable points
  //  - The handler-is-null case should be as fast as we can make it
  //
  // The scratch register is available here.
  //
  // An unconditional callout would be densest but is too slow.  The best
  // balance results from an inline test for null with a conditional call.  The
  // best code sequence is platform-dependent.
  //
  // The conditional call goes to a stub attached to the function that performs
  // further filtering before calling the breakpoint handler.
#if defined(JS_CODEGEN_X64)
  // REX 83 MODRM OFFS IB
  static_assert(Instance::offsetOfDebugStub() < 128);
  masm.cmpq(Imm32(0),
            Operand(Address(InstanceReg, Instance::offsetOfDebugStub())));

  // 74 OFFS
  Label L;
  L.bind(masm.currentOffset() + 7);
  masm.j(Assembler::Zero, &L);

  // E8 OFFS OFFS OFFS OFFS
  masm.call(&perFunctionDebugStub_);
  masm.append(CallSiteDesc(currentOpOffset_, kind),
              CodeOffset(masm.currentOffset()));

  // Branch destination
  MOZ_ASSERT_IF(!masm.oom(), masm.currentOffset() == uint32_t(L.offset()));
#elif defined(JS_CODEGEN_X86)
  // 83 MODRM OFFS IB
  static_assert(Instance::offsetOfDebugStub() < 128);
  masm.cmpl(Imm32(0),
            Operand(Address(InstanceReg, Instance::offsetOfDebugStub())));

  // 74 OFFS
  Label L;
  L.bind(masm.currentOffset() + 7);
  masm.j(Assembler::Zero, &L);

  // E8 OFFS OFFS OFFS OFFS
  masm.call(&perFunctionDebugStub_);
  masm.append(CallSiteDesc(currentOpOffset_, kind),
              CodeOffset(masm.currentOffset()));

  // Branch destination
  MOZ_ASSERT_IF(!masm.oom(), masm.currentOffset() == uint32_t(L.offset()));
#elif defined(JS_CODEGEN_ARM64)
  ScratchPtr scratch(*this);
  ARMRegister tmp(scratch, 64);
  Label L;
  masm.Ldr(tmp,
           MemOperand(Address(InstanceReg, Instance::offsetOfDebugStub())));
  masm.Cbz(tmp, &L);
  masm.Bl(&perFunctionDebugStub_);
  masm.append(CallSiteDesc(currentOpOffset_, kind),
              CodeOffset(masm.currentOffset()));
  masm.bind(&L);
#elif defined(JS_CODEGEN_ARM)
  ScratchPtr scratch(*this);
  masm.loadPtr(Address(InstanceReg, Instance::offsetOfDebugStub()), scratch);
  masm.ma_orr(scratch, scratch, SetCC);
  masm.ma_bl(&perFunctionDebugStub_, Assembler::NonZero);
  masm.append(CallSiteDesc(currentOpOffset_, kind),
              CodeOffset(masm.currentOffset()));
#elif defined(JS_CODEGEN_LOONG64) || defined(JS_CODEGEN_MIPS64) || \
    defined(JS_CODEGEN_RISCV64)
  ScratchPtr scratch(*this);
  Label L;
  masm.loadPtr(Address(InstanceReg, Instance::offsetOfDebugStub()), scratch);
  masm.branchPtr(Assembler::Equal, scratch, ImmWord(0), &L);
  const CodeOffset retAddr = masm.call(&perFunctionDebugStub_);
  masm.append(CallSiteDesc(currentOpOffset_, kind), retAddr);
  masm.bind(&L);
#else
  MOZ_CRASH("BaseCompiler platform hook: insertBreakablePoint");
#endif
}

void BaseCompiler::insertPerFunctionDebugStub() {
  // The per-function debug stub performs out-of-line filtering before jumping
  // to the per-module debug stub if necessary.  The per-module debug stub
  // returns directly to the breakable point.
  //
  // NOTE, the link register is live here on platforms that have LR.
  //
  // The scratch register is available here (as it was at the call site).
  //
  // It's useful for the per-function debug stub to be compact, as every
  // function gets one.

  Label L;
  masm.bind(&perFunctionDebugStub_);

#if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
  {
    ScratchPtr scratch(*this);

    // Get the per-instance table of filtering bits.
    masm.loadPtr(Address(InstanceReg, Instance::offsetOfDebugFilter()),
                 scratch);

    // Check the filter bit.  There is one bit per function in the module.
    // Table elements are 32-bit because the masm makes that convenient.
    masm.branchTest32(Assembler::NonZero,
                      Address(scratch, (func_.index / 32) * sizeof(uint32_t)),
                      Imm32(1 << (func_.index % 32)), &L);

    // Fast path: return to the execution.
    masm.ret();
  }
#elif defined(JS_CODEGEN_ARM64)
  {
    ScratchPtr scratch(*this);

    // Logic as above, except abiret to jump to the LR directly
    masm.loadPtr(Address(InstanceReg, Instance::offsetOfDebugFilter()),
                 scratch);
    masm.branchTest32(Assembler::NonZero,
                      Address(scratch, (func_.index / 32) * sizeof(uint32_t)),
                      Imm32(1 << (func_.index % 32)), &L);
    masm.abiret();
  }
#elif defined(JS_CODEGEN_ARM)
  {
    // We must be careful not to use the SecondScratchRegister, which usually
    // is LR, as LR is live here.  This means avoiding masm abstractions such
    // as branchTest32.

    static_assert(ScratchRegister != lr);
    static_assert(Instance::offsetOfDebugFilter() < 0x1000);

    ScratchRegisterScope tmp1(masm);
    ScratchI32 tmp2(*this);
    masm.ma_ldr(
        DTRAddr(InstanceReg, DtrOffImm(Instance::offsetOfDebugFilter())), tmp1);
    masm.ma_mov(Imm32(func_.index / 32), tmp2);
    masm.ma_ldr(DTRAddr(tmp1, DtrRegImmShift(tmp2, LSL, 2)), tmp2);
    masm.ma_tst(tmp2, Imm32(1 << func_.index % 32), tmp1, Assembler::Always);
    masm.ma_bx(lr, Assembler::Zero);
  }
#elif defined(JS_CODEGEN_LOONG64) || defined(JS_CODEGEN_MIPS64) || \
    defined(JS_CODEGEN_RISCV64)
  {
    ScratchPtr scratch(*this);

    // Logic same as ARM64.
    masm.loadPtr(Address(InstanceReg, Instance::offsetOfDebugFilter()),
                 scratch);
    masm.branchTest32(Assembler::NonZero,
                      Address(scratch, (func_.index / 32) * sizeof(uint32_t)),
                      Imm32(1 << (func_.index % 32)), &L);
    masm.abiret();
  }
#else
  MOZ_CRASH("BaseCompiler platform hook: endFunction");
#endif

  // Jump to the per-module debug stub, which calls onwards to C++ land.
  masm.bind(&L);
  masm.jump(Address(InstanceReg, Instance::offsetOfDebugStub()));
}

void BaseCompiler::saveRegisterReturnValues(const ResultType& resultType) {
  MOZ_ASSERT(compilerEnv_.debugEnabled());
  size_t debugFrameOffset = masm.framePushed() - DebugFrame::offsetOfFrame();
  size_t registerResultIdx = 0;
  for (ABIResultIter i(resultType); !i.done(); i.next()) {
    const ABIResult result = i.cur();
    if (!result.inRegister()) {
#ifdef DEBUG
      for (i.next(); !i.done(); i.next()) {
        MOZ_ASSERT(!i.cur().inRegister());
      }
#endif
      break;
    }

    size_t resultOffset = DebugFrame::offsetOfRegisterResult(registerResultIdx);
    Address dest(masm.getStackPointer(), debugFrameOffset + resultOffset);
    switch (result.type().kind()) {
      case ValType::I32:
        masm.store32(RegI32(result.gpr()), dest);
        break;
      case ValType::I64:
        masm.store64(RegI64(result.gpr64()), dest);
        break;
      case ValType::F64:
        masm.storeDouble(RegF64(result.fpr()), dest);
        break;
      case ValType::F32:
        masm.storeFloat32(RegF32(result.fpr()), dest);
        break;
      case ValType::Ref: {
        uint32_t flag =
            DebugFrame::hasSpilledRegisterRefResultBitMask(registerResultIdx);
        // Tell Instance::traceFrame that we have a pointer to trace.
        masm.or32(Imm32(flag),
                  Address(masm.getStackPointer(),
                          debugFrameOffset + DebugFrame::offsetOfFlags()));
        masm.storePtr(RegRef(result.gpr()), dest);
        break;
      }
      case ValType::V128:
#ifdef ENABLE_WASM_SIMD
        masm.storeUnalignedSimd128(RegV128(result.fpr()), dest);
        break;
#else
        MOZ_CRASH("No SIMD support");
#endif
    }
    registerResultIdx++;
  }
}

void BaseCompiler::restoreRegisterReturnValues(const ResultType& resultType) {
  MOZ_ASSERT(compilerEnv_.debugEnabled());
  size_t debugFrameOffset = masm.framePushed() - DebugFrame::offsetOfFrame();
  size_t registerResultIdx = 0;
  for (ABIResultIter i(resultType); !i.done(); i.next()) {
    const ABIResult result = i.cur();
    if (!result.inRegister()) {
#ifdef DEBUG
      for (i.next(); !i.done(); i.next()) {
        MOZ_ASSERT(!i.cur().inRegister());
      }
#endif
      break;
    }
    size_t resultOffset =
        DebugFrame::offsetOfRegisterResult(registerResultIdx++);
    Address src(masm.getStackPointer(), debugFrameOffset + resultOffset);
    switch (result.type().kind()) {
      case ValType::I32:
        masm.load32(src, RegI32(result.gpr()));
        break;
      case ValType::I64:
        masm.load64(src, RegI64(result.gpr64()));
        break;
      case ValType::F64:
        masm.loadDouble(src, RegF64(result.fpr()));
        break;
      case ValType::F32:
        masm.loadFloat32(src, RegF32(result.fpr()));
        break;
      case ValType::Ref:
        masm.loadPtr(src, RegRef(result.gpr()));
        break;
      case ValType::V128:
#ifdef ENABLE_WASM_SIMD
        masm.loadUnalignedSimd128(src, RegV128(result.fpr()));
        break;
#else
        MOZ_CRASH("No SIMD support");
#endif
    }
  }
}

//////////////////////////////////////////////////////////////////////////////
//
// Support for lazy tiering

// The key thing here is, we generate a short piece of code which, most of the
// time, has no effect, but just occasionally wants to call out to C++ land.
// That's a similar requirement to the Debugger API support (see above) and so
// we have a similar, but simpler, solution.  Specifically, we use a single
// stub routine for the whole module, whereas for debugging, there are
// per-function stub routines as well as a whole-module stub routine involved.

class OutOfLineRequestTierUp : public OutOfLineCode {
  Register instance_;  // points at the instance at entry; must remain unchanged
  Maybe<RegI32> scratch_;    // only provided on arm32
  size_t lastOpcodeOffset_;  // a bytecode offset

 public:
  OutOfLineRequestTierUp(Register instance, Maybe<RegI32> scratch,
                         size_t lastOpcodeOffset)
      : instance_(instance),
        scratch_(scratch),
        lastOpcodeOffset_(lastOpcodeOffset) {}
  virtual void generate(MacroAssembler* masm, BaseCompiler* bc) override {
    // Generate:
    //
    // [optionally, if `instance_` != InstanceReg: swap(instance_, InstanceReg)]
    // call * $offsetOfRequestTierUpStub(InstanceReg)
    // [optionally, if `instance_` != InstanceReg: swap(instance_, InstanceReg)]
    // goto rejoin
    //
    // This is the unlikely path, where we call the (per-module)
    // request-tier-up stub.  The stub wants the instance pointer to be in the
    // official InstanceReg at this point, but InstanceReg itself might hold
    // arbitrary other live data.  Hence, if necessary, swap `instance_` and
    // InstanceReg before the call and swap them back after it.
#ifndef RABALDR_PIN_INSTANCE
    if (Register(instance_) != InstanceReg) {
#  ifdef JS_CODEGEN_X86
      // On x86_32 this is easy.
      masm->xchgl(instance_, InstanceReg);
#  elif JS_CODEGEN_ARM
      masm->mov(instance_,
                scratch_.value());  // note, destination is second arg
      masm->mov(InstanceReg, instance_);
      masm->mov(scratch_.value(), InstanceReg);
#  else
      MOZ_CRASH("BaseCompiler::OutOfLineRequestTierUp #1");
#  endif
    }
#endif
    // Call the stub
    const CodeOffset retAddr =
        masm->call(Address(InstanceReg, Instance::offsetOfRequestTierUpStub()));
    masm->append(CallSiteDesc(lastOpcodeOffset_, CallSiteKind::RequestTierUp),
                 retAddr);
    // And swap again, if we swapped above.
#ifndef RABALDR_PIN_INSTANCE
    if (Register(instance_) != InstanceReg) {
#  ifdef JS_CODEGEN_X86
      masm->xchgl(instance_, InstanceReg);
#  elif JS_CODEGEN_ARM
      masm->mov(instance_, scratch_.value());
      masm->mov(InstanceReg, instance_);
      masm->mov(scratch_.value(), InstanceReg);
#  else
      MOZ_CRASH("BaseCompiler::OutOfLineRequestTierUp #2");
#  endif
    }
#endif

    masm->jump(rejoin());
  }
};

Maybe<CodeOffset> BaseCompiler::addHotnessCheck() {
  // Here's an example of what we'll create.  The path that almost always
  // happens, where the counter doesn't go negative, has just one branch.
  //
  //   subl       $to_be_filled_in_later, 0x170(%r14)
  //   js         oolCode // almost never taken
  // rejoin:
  // ----------------
  // oolCode: // we get here when the counter is negative, viz, almost never
  //   call       *0x160(%r14) // RequestTierUpStub
  //   jmp        rejoin
  //
  // Note that the counter is updated regardless of whether or not it has gone
  // negative.  That means that, at entry to RequestTierUpStub, we know the
  // counter must be negative, and not merely zero.
  //
  // Non-Intel targets will have to generate a load / subtract-and-set-flags /
  // store / jcond sequence.
  //
  // To ensure the shortest possible encoding, `to_be_filled_in_later` must be
  // a value in the range 1 .. 127 inclusive.  This is good enough for
  // hotness-counting purposes.

  AutoCreatedBy acb(masm, "BC::addHotnessCheck");

  // A sync() must happen before this. The request tier-up stub does not save
  // all live registers.
  MOZ_ASSERT(!hasLiveRegsOnStk());

#ifdef RABALDR_PIN_INSTANCE
  Register instance(InstanceReg);
#else
  // This seems to assume that any non-RABALDR_PIN_INSTANCE target is 32-bit
  ScratchI32 instance(*this);
  fr.loadInstancePtr(instance);
#endif

  Address addressOfCounter = Address(
      instance, wasm::Instance::offsetInData(
                    codeMeta_.offsetOfFuncDefInstanceData(func_.index)));

#if JS_CODEGEN_ARM
  Maybe<RegI32> scratch = Some(needI32());
#else
  Maybe<RegI32> scratch = Nothing();
#endif

  OutOfLineCode* ool = addOutOfLineCode(new (alloc_) OutOfLineRequestTierUp(
      instance, scratch, currentOpOffset_));
  if (!ool) {
    return Nothing();
  }

  // Because of the Intel arch instruction formats, `patchPoint` points to the
  // byte immediately following the last byte of the instruction to patch.
  CodeOffset patchPoint = masm.sub32FromMemAndBranchIfNegativeWithPatch(
      addressOfCounter, ool->entry());

  masm.bind(ool->rejoin());

  if (scratch.isSome()) {
    freeI32(scratch.value());
  }

  // `patchPoint` might be invalid if the assembler OOMd at some point.
  return masm.oom() ? Nothing() : Some(patchPoint);
}

void BaseCompiler::patchHotnessCheck(CodeOffset offset, uint32_t step) {
  // Zero makes the hotness check pointless.  Above 127 is not representable in
  // the short-form Intel encoding.
  MOZ_RELEASE_ASSERT(step > 0 && step <= 127);
  MOZ_ASSERT(!masm.oom());
  masm.patchSub32FromMemAndBranchIfNegative(offset, Imm32(step));
}

//////////////////////////////////////////////////////////////////////////////
//
// Results and block parameters

void BaseCompiler::popStackReturnValues(const ResultType& resultType) {
  uint32_t bytes = ABIResultIter::MeasureStackBytes(resultType);
  if (bytes == 0) {
    return;
  }
  Register target = ABINonArgReturnReg0;
  Register temp = ABINonArgReturnReg1;
  fr.loadIncomingStackResultAreaPtr(RegPtr(target));
  fr.popStackResultsToMemory(target, bytes, temp);
}

// TODO / OPTIMIZE (Bug 1316818): At the moment we use the Wasm
// inter-procedure ABI for block returns, which allocates ReturnReg as the
// single block result register.  It is possible other choices would lead to
// better register allocation, as ReturnReg is often first in the register set
// and will be heavily wanted by the register allocator that uses takeFirst().
//
// Obvious options:
//  - pick a register at the back of the register set
//  - pick a random register per block (different blocks have
//    different join regs)

void BaseCompiler::popRegisterResults(ABIResultIter& iter) {
  // Pop register results.  Note that in the single-value case, popping to a
  // register may cause a sync(); for multi-value we sync'd already.
  for (; !iter.done(); iter.next()) {
    const ABIResult& result = iter.cur();
    if (!result.inRegister()) {
      // TODO / OPTIMIZE: We sync here to avoid solving the general parallel
      // move problem in popStackResults.  However we could avoid syncing the
      // values that are going to registers anyway, if they are already in
      // registers.
      sync();
      break;
    }
    switch (result.type().kind()) {
      case ValType::I32:
        popI32(RegI32(result.gpr()));
        break;
      case ValType::I64:
        popI64(RegI64(result.gpr64()));
        break;
      case ValType::F32:
        popF32(RegF32(result.fpr()));
        break;
      case ValType::F64:
        popF64(RegF64(result.fpr()));
        break;
      case ValType::Ref:
        popRef(RegRef(result.gpr()));
        break;
      case ValType::V128:
#ifdef ENABLE_WASM_SIMD
        popV128(RegV128(result.fpr()));
#else
        MOZ_CRASH("No SIMD support");
#endif
    }
  }
}

void BaseCompiler::popStackResults(ABIResultIter& iter, StackHeight stackBase) {
  MOZ_ASSERT(!iter.done());

  // The iterator should be advanced beyond register results, and register
  // results should be popped already from the value stack.
  uint32_t alreadyPopped = iter.index();

  // At this point, only stack arguments are remaining.  Iterate through them
  // to measure how much stack space they will take up.
  for (; !iter.done(); iter.next()) {
    MOZ_ASSERT(iter.cur().onStack());
  }

  // Calculate the space needed to store stack results, in bytes.
  uint32_t stackResultBytes = iter.stackBytesConsumedSoFar();
  MOZ_ASSERT(stackResultBytes);

  // Compute the stack height including the stack results.  Note that it's
  // possible that this call expands the stack, for example if some of the
  // results are supplied by constants and so are not already on the machine
  // stack.
  uint32_t endHeight = fr.prepareStackResultArea(stackBase, stackResultBytes);

  // Find a free GPR to use when shuffling stack values.  If none is
  // available, push ReturnReg and restore it after we're done.
  bool saved = false;
  RegPtr temp = ra.needTempPtr(RegPtr(ReturnReg), &saved);

  // The sequence of Stk values is in the same order on the machine stack as
  // the result locations, but there is a complication: constant values are
  // not actually pushed on the machine stack.  (At this point registers and
  // locals have been spilled already.)  So, moving the Stk values into place
  // isn't simply a shuffle-down or shuffle-up operation.  There is a part of
  // the Stk sequence that shuffles toward the FP, a part that's already in
  // place, and a part that shuffles toward the SP.  After shuffling, we have
  // to materialize the constants.

  // Shuffle mem values toward the frame pointer, copying deepest values
  // first.  Stop when we run out of results, get to a register result, or
  // find a Stk value that is closer to the FP than the result.
  for (iter.switchToPrev(); !iter.done(); iter.prev()) {
    const ABIResult& result = iter.cur();
    if (!result.onStack()) {
      break;
    }
    MOZ_ASSERT(result.stackOffset() < stackResultBytes);
    uint32_t destHeight = endHeight - result.stackOffset();
    uint32_t stkBase = stk_.length() - (iter.count() - alreadyPopped);
    Stk& v = stk_[stkBase + iter.index()];
    if (v.isMem()) {
      uint32_t srcHeight = v.offs();
      if (srcHeight <= destHeight) {
        break;
      }
      fr.shuffleStackResultsTowardFP(srcHeight, destHeight, result.size(),
                                     temp);
    }
  }

  // Reset iterator and skip register results.
  for (iter.reset(); !iter.done(); iter.next()) {
    if (iter.cur().onStack()) {
      break;
    }
  }

  // Revisit top stack values, shuffling mem values toward the stack pointer,
  // copying shallowest values first.
  for (; !iter.done(); iter.next()) {
    const ABIResult& result = iter.cur();
    MOZ_ASSERT(result.onStack());
    MOZ_ASSERT(result.stackOffset() < stackResultBytes);
    uint32_t destHeight = endHeight - result.stackOffset();
    Stk& v = stk_[stk_.length() - (iter.index() - alreadyPopped) - 1];
    if (v.isMem()) {
      uint32_t srcHeight = v.offs();
      if (srcHeight >= destHeight) {
        break;
      }
      fr.shuffleStackResultsTowardSP(srcHeight, destHeight, result.size(),
                                     temp);
    }
  }

  // Reset iterator and skip register results, which are already popped off
  // the value stack.
  for (iter.reset(); !iter.done(); iter.next()) {
    if (iter.cur().onStack()) {
      break;
    }
  }

  // Materialize constants and pop the remaining items from the value stack.
  for (; !iter.done(); iter.next()) {
    const ABIResult& result = iter.cur();
    uint32_t resultHeight = endHeight - result.stackOffset();
    Stk& v = stk_.back();
    switch (v.kind()) {
      case Stk::ConstI32:
#if defined(JS_CODEGEN_MIPS64) || defined(JS_CODEGEN_LOONG64) || \
    defined(JS_CODEGEN_RISCV64)
        fr.storeImmediatePtrToStack(v.i32val_, resultHeight, temp);
#else
        fr.storeImmediatePtrToStack(uint32_t(v.i32val_), resultHeight, temp);
#endif
        break;
      case Stk::ConstF32:
        fr.storeImmediateF32ToStack(v.f32val_, resultHeight, temp);
        break;
      case Stk::ConstI64:
        fr.storeImmediateI64ToStack(v.i64val_, resultHeight, temp);
        break;
      case Stk::ConstF64:
        fr.storeImmediateF64ToStack(v.f64val_, resultHeight, temp);
        break;
#ifdef ENABLE_WASM_SIMD
      case Stk::ConstV128:
        fr.storeImmediateV128ToStack(v.v128val_, resultHeight, temp);
        break;
#endif
      case Stk::ConstRef:
        fr.storeImmediatePtrToStack(v.refval_, resultHeight, temp);
        break;
      case Stk::MemRef:
        // Update bookkeeping as we pop the Stk entry.
        stackMapGenerator_.memRefsOnStk--;
        break;
      default:
        MOZ_ASSERT(v.isMem());
        break;
    }
    stk_.popBack();
  }

  ra.freeTempPtr(temp, saved);

  // This will pop the stack if needed.
  fr.finishStackResultArea(stackBase, stackResultBytes);
}

void BaseCompiler::popBlockResults(ResultType type, StackHeight stackBase,
                                   ContinuationKind kind) {
  if (!type.empty()) {
    ABIResultIter iter(type);
    popRegisterResults(iter);
    if (!iter.done()) {
      popStackResults(iter, stackBase);
      // Because popStackResults might clobber the stack, it leaves the stack
      // pointer already in the right place for the continuation, whether the
      // continuation is a jump or fallthrough.
      return;
    }
  }
  // We get here if there are no stack results.  For a fallthrough, the stack
  // is already at the right height.  For a jump, we may need to pop the stack
  // pointer if the continuation's stack height is lower than the current
  // stack height.
  if (kind == ContinuationKind::Jump) {
    fr.popStackBeforeBranch(stackBase, type);
  }
}

// This function is similar to popBlockResults, but additionally handles the
// implicit exception pointer that is pushed to the value stack on entry to
// a catch handler by dropping it appropriately.
void BaseCompiler::popCatchResults(ResultType type, StackHeight stackBase) {
  if (!type.empty()) {
    ABIResultIter iter(type);
    popRegisterResults(iter);
    if (!iter.done()) {
      popStackResults(iter, stackBase);
      // Since popStackResults clobbers the stack, we only need to free the
      // exception off of the value stack.
      popValueStackBy(1);
    } else {
      // If there are no stack results, we have to adjust the stack by
      // dropping the exception reference that's now on the stack.
      dropValue();
    }
  } else {
    dropValue();
  }
  fr.popStackBeforeBranch(stackBase, type);
}

Stk BaseCompiler::captureStackResult(const ABIResult& result,
                                     StackHeight resultsBase,
                                     uint32_t stackResultBytes) {
  MOZ_ASSERT(result.onStack());
  uint32_t offs = fr.locateStackResult(result, resultsBase, stackResultBytes);
  return Stk::StackResult(result.type(), offs);
}

// TODO: It may be fruitful to inline the fast path here, as it will be common.

bool BaseCompiler::pushResults(ResultType type, StackHeight resultsBase) {
  if (type.empty()) {
    return true;
  }

  if (type.length() > 1) {
    // Reserve extra space on the stack for all the values we'll push.
    // Multi-value push is not accounted for by the pre-sizing of the stack in
    // the decoding loop.
    //
    // Also make sure we leave headroom for other pushes that will occur after
    // pushing results, just to be safe.
    if (!stk_.reserve(stk_.length() + type.length() + MaxPushesPerOpcode)) {
      return false;
    }
  }

  // We need to push the results in reverse order, so first iterate through
  // all results to determine the locations of stack result types.
  ABIResultIter iter(type);
  while (!iter.done()) {
    iter.next();
  }
  uint32_t stackResultBytes = iter.stackBytesConsumedSoFar();
  for (iter.switchToPrev(); !iter.done(); iter.prev()) {
    const ABIResult& result = iter.cur();
    if (!result.onStack()) {
      break;
    }
    Stk v = captureStackResult(result, resultsBase, stackResultBytes);
    push(v);
    if (v.kind() == Stk::MemRef) {
      stackMapGenerator_.memRefsOnStk++;
    }
  }

  for (; !iter.done(); iter.prev()) {
    const ABIResult& result = iter.cur();
    MOZ_ASSERT(result.inRegister());
    switch (result.type().kind()) {
      case ValType::I32:
        pushI32(RegI32(result.gpr()));
        break;
      case ValType::I64:
        pushI64(RegI64(result.gpr64()));
        break;
      case ValType::V128:
#ifdef ENABLE_WASM_SIMD
        pushV128(RegV128(result.fpr()));
        break;
#else
        MOZ_CRASH("No SIMD support");
#endif
      case ValType::F32:
        pushF32(RegF32(result.fpr()));
        break;
      case ValType::F64:
        pushF64(RegF64(result.fpr()));
        break;
      case ValType::Ref:
        pushRef(RegRef(result.gpr()));
        break;
    }
  }

  return true;
}

bool BaseCompiler::pushBlockResults(ResultType type) {
  return pushResults(type, controlItem().stackHeight);
}

// A combination of popBlockResults + pushBlockResults, used when entering a
// block with a control-flow join (loops) or split (if) to shuffle the
// fallthrough block parameters into the locations expected by the
// continuation.
bool BaseCompiler::topBlockParams(ResultType type) {
  // This function should only be called when entering a block with a
  // control-flow join at the entry, where there are no live temporaries in
  // the current block.
  StackHeight base = controlItem().stackHeight;
  MOZ_ASSERT(fr.stackResultsBase(stackConsumed(type.length())) == base);
  popBlockResults(type, base, ContinuationKind::Fallthrough);
  return pushBlockResults(type);
}

// A combination of popBlockResults + pushBlockResults, used before branches
// where we don't know the target (br_if / br_table).  If and when the branch
// is taken, the stack results will be shuffled down into place.  For br_if
// that has fallthrough, the parameters for the untaken branch flow through to
// the continuation.
bool BaseCompiler::topBranchParams(ResultType type, StackHeight* height) {
  if (type.empty()) {
    *height = fr.stackHeight();
    return true;
  }
  // There may be temporary values that need spilling; delay computation of
  // the stack results base until after the popRegisterResults(), which spills
  // if needed.
  ABIResultIter iter(type);
  popRegisterResults(iter);
  StackHeight base = fr.stackResultsBase(stackConsumed(iter.remaining()));
  if (!iter.done()) {
    popStackResults(iter, base);
  }
  if (!pushResults(type, base)) {
    return false;
  }
  *height = base;
  return true;
}

// Conditional branches with fallthrough are preceded by a topBranchParams, so
// we know that there are no stack results that need to be materialized.  In
// that case, we can just shuffle the whole block down before popping the
// stack.
void BaseCompiler::shuffleStackResultsBeforeBranch(StackHeight srcHeight,
                                                   StackHeight destHeight,
                                                   ResultType type) {
  uint32_t stackResultBytes = 0;

  if (ABIResultIter::HasStackResults(type)) {
    MOZ_ASSERT(stk_.length() >= type.length());
    ABIResultIter iter(type);
    for (; !iter.done(); iter.next()) {
#ifdef DEBUG
      const ABIResult& result = iter.cur();
      const Stk& v = stk_[stk_.length() - iter.index() - 1];
      MOZ_ASSERT(v.isMem() == result.onStack());
#endif
    }

    stackResultBytes = iter.stackBytesConsumedSoFar();
    MOZ_ASSERT(stackResultBytes > 0);

    if (srcHeight != destHeight) {
      // Find a free GPR to use when shuffling stack values.  If none
      // is available, push ReturnReg and restore it after we're done.
      bool saved = false;
      RegPtr temp = ra.needTempPtr(RegPtr(ReturnReg), &saved);
      fr.shuffleStackResultsTowardFP(srcHeight, destHeight, stackResultBytes,
                                     temp);
      ra.freeTempPtr(temp, saved);
    }
  }

  fr.popStackBeforeBranch(destHeight, stackResultBytes);
}

bool BaseCompiler::insertDebugCollapseFrame() {
  if (!compilerEnv_.debugEnabled() || deadCode_) {
    return true;
  }

  insertBreakablePoint(CallSiteKind::CollapseFrame);
  // stackmap for: debug: collapse-frame breakpoint
  return createStackMap(Nothing() /* stackmap pertains to a call */,
                        HasDebugFrameWithLiveRefs::Maybe);
}

//////////////////////////////////////////////////////////////////////////////
//
// Function calls.

void BaseCompiler::beginCall(FunctionCall& call) {
  // Use masm.framePushed() because the value we want here does not depend
  // on the height of the frame's stack area, but the actual size of the
  // allocated frame.
  call.frameAlignAdjustment = ComputeByteAlignment(
      masm.framePushed() + sizeof(Frame), JitStackAlignment);
}

void BaseCompiler::endCall(FunctionCall& call, size_t stackSpace) {
  size_t adjustment = call.stackArgAreaSize + call.frameAlignAdjustment;
  fr.freeArgAreaAndPopBytes(adjustment, stackSpace);

  MOZ_ASSERT(stackMapGenerator_.framePushedExcludingOutboundCallArgs.isSome());
  stackMapGenerator_.framePushedExcludingOutboundCallArgs.reset();

  if (call.restoreState == RestoreState::All) {
    fr.loadInstancePtr(InstanceReg);
    masm.loadWasmPinnedRegsFromInstance(mozilla::Nothing());
    masm.switchToWasmInstanceRealm(ABINonArgReturnReg0, ABINonArgReturnReg1);
  } else if (call.restoreState == RestoreState::PinnedRegs) {
    masm.loadWasmPinnedRegsFromInstance(mozilla::Nothing());
  }
}

void BaseCompiler::startCallArgs(size_t stackArgAreaSizeUnaligned,
                                 FunctionCall* call) {
  size_t stackArgAreaSizeAligned =
      AlignStackArgAreaSize(stackArgAreaSizeUnaligned);
  MOZ_ASSERT(stackArgAreaSizeUnaligned <= stackArgAreaSizeAligned);

  // Record the masm.framePushed() value at this point, before we push args
  // for the call and any required alignment space. This defines the lower limit
  // of the stackmap that will be created for this call.
  MOZ_ASSERT(
      stackMapGenerator_.framePushedExcludingOutboundCallArgs.isNothing());
  stackMapGenerator_.framePushedExcludingOutboundCallArgs.emplace(
      // However much we've pushed so far
      masm.framePushed() +
      // Extra space we'll push to get the frame aligned
      call->frameAlignAdjustment);

  call->stackArgAreaSize = stackArgAreaSizeAligned;

  size_t adjustment = call->stackArgAreaSize + call->frameAlignAdjustment;
  fr.allocArgArea(adjustment);
}

ABIArg BaseCompiler::reservePointerArgument(FunctionCall* call) {
  return call->abi.next(MIRType::Pointer);
}

void BaseCompiler::passArg(ValType type, const Stk& arg, FunctionCall* call) {
  switch (type.kind()) {
    case ValType::I32: {
      ABIArg argLoc = call->abi.next(MIRType::Int32);
      if (argLoc.kind() == ABIArg::Stack) {
        ScratchI32 scratch(*this);
        loadI32(arg, scratch);
        masm.store32(scratch, Address(masm.getStackPointer(),
                                      argLoc.offsetFromArgBase()));
      } else {
        loadI32(arg, RegI32(argLoc.gpr()));
      }
      break;
    }
    case ValType::I64: {
      ABIArg argLoc = call->abi.next(MIRType::Int64);
      if (argLoc.kind() == ABIArg::Stack) {
        ScratchI32 scratch(*this);
#ifdef JS_PUNBOX64
        loadI64(arg, fromI32(scratch));
        masm.storePtr(scratch, Address(masm.getStackPointer(),
                                       argLoc.offsetFromArgBase()));
#else
        loadI64Low(arg, scratch);
        masm.store32(scratch, LowWord(Address(masm.getStackPointer(),
                                              argLoc.offsetFromArgBase())));
        loadI64High(arg, scratch);
        masm.store32(scratch, HighWord(Address(masm.getStackPointer(),
                                               argLoc.offsetFromArgBase())));
#endif
      } else {
        loadI64(arg, RegI64(argLoc.gpr64()));
      }
      break;
    }
    case ValType::V128: {
#ifdef ENABLE_WASM_SIMD
      ABIArg argLoc = call->abi.next(MIRType::Simd128);
      switch (argLoc.kind()) {
        case ABIArg::Stack: {
          ScratchV128 scratch(*this);
          loadV128(arg, scratch);
          masm.storeUnalignedSimd128(
              (RegV128)scratch,
              Address(masm.getStackPointer(), argLoc.offsetFromArgBase()));
          break;
        }
        case ABIArg::GPR: {
          MOZ_CRASH("Unexpected parameter passing discipline");
        }
        case ABIArg::FPU: {
          loadV128(arg, RegV128(argLoc.fpu()));
          break;
        }
#  if defined(JS_CODEGEN_REGISTER_PAIR)
        case ABIArg::GPR_PAIR: {
          MOZ_CRASH("Unexpected parameter passing discipline");
        }
#  endif
        case ABIArg::Uninitialized:
          MOZ_CRASH("Uninitialized ABIArg kind");
      }
      break;
#else
      MOZ_CRASH("No SIMD support");
#endif
    }
    case ValType::F64: {
      ABIArg argLoc = call->abi.next(MIRType::Double);
      switch (argLoc.kind()) {
        case ABIArg::Stack: {
          ScratchF64 scratch(*this);
          loadF64(arg, scratch);
          masm.storeDouble(scratch, Address(masm.getStackPointer(),
                                            argLoc.offsetFromArgBase()));
          break;
        }
#if defined(JS_CODEGEN_REGISTER_PAIR)
        case ABIArg::GPR_PAIR: {
#  if defined(JS_CODEGEN_ARM)
          ScratchF64 scratch(*this);
          loadF64(arg, scratch);
          masm.ma_vxfer(scratch, argLoc.evenGpr(), argLoc.oddGpr());
          break;
#  else
          MOZ_CRASH("BaseCompiler platform hook: passArg F64 pair");
#  endif
        }
#endif
        case ABIArg::FPU: {
          loadF64(arg, RegF64(argLoc.fpu()));
          break;
        }
        case ABIArg::GPR: {
          MOZ_CRASH("Unexpected parameter passing discipline");
        }
        case ABIArg::Uninitialized:
          MOZ_CRASH("Uninitialized ABIArg kind");
      }
      break;
    }
    case ValType::F32: {
      ABIArg argLoc = call->abi.next(MIRType::Float32);
      switch (argLoc.kind()) {
        case ABIArg::Stack: {
          ScratchF32 scratch(*this);
          loadF32(arg, scratch);
          masm.storeFloat32(scratch, Address(masm.getStackPointer(),
                                             argLoc.offsetFromArgBase()));
          break;
        }
        case ABIArg::GPR: {
          ScratchF32 scratch(*this);
          loadF32(arg, scratch);
          masm.moveFloat32ToGPR(scratch, argLoc.gpr());
          break;
        }
        case ABIArg::FPU: {
          loadF32(arg, RegF32(argLoc.fpu()));
          break;
        }
#if defined(JS_CODEGEN_REGISTER_PAIR)
        case ABIArg::GPR_PAIR: {
          MOZ_CRASH("Unexpected parameter passing discipline");
        }
#endif
        case ABIArg::Uninitialized:
          MOZ_CRASH("Uninitialized ABIArg kind");
      }
      break;
    }
    case ValType::Ref: {
      ABIArg argLoc = call->abi.next(MIRType::WasmAnyRef);
      if (argLoc.kind() == ABIArg::Stack) {
        ScratchRef scratch(*this);
        loadRef(arg, scratch);
        masm.storePtr(scratch, Address(masm.getStackPointer(),
                                       argLoc.offsetFromArgBase()));
      } else {
        loadRef(arg, RegRef(argLoc.gpr()));
      }
      break;
    }
  }
}

template <typename T>
bool BaseCompiler::emitCallArgs(const ValTypeVector& argTypes, T results,
                                FunctionCall* baselineCall,
                                CalleeOnStack calleeOnStack) {
  MOZ_ASSERT(!deadCode_);

  ArgTypeVector args(argTypes, results.stackResults());
  uint32_t naturalArgCount = argTypes.length();
  uint32_t abiArgCount = args.lengthWithStackResults();
  startCallArgs(StackArgAreaSizeUnaligned(args, baselineCall->abiKind),
                baselineCall);

  // Args are deeper on the stack than the stack result area, if any.
  size_t argsDepth = results.onStackCount();
  // They're deeper than the callee too, for callIndirect.
  if (calleeOnStack == CalleeOnStack::True) {
    argsDepth++;
  }

  for (size_t i = 0; i < abiArgCount; ++i) {
    if (args.isNaturalArg(i)) {
      size_t naturalIndex = args.naturalIndex(i);
      size_t stackIndex = naturalArgCount - 1 - naturalIndex + argsDepth;
      passArg(argTypes[naturalIndex], peek(stackIndex), baselineCall);
    } else {
      // The synthetic stack result area pointer.
      ABIArg argLoc = baselineCall->abi.next(MIRType::Pointer);
      if (argLoc.kind() == ABIArg::Stack) {
        ScratchPtr scratch(*this);
        results.getStackResultArea(fr, scratch);
        masm.storePtr(scratch, Address(masm.getStackPointer(),
                                       argLoc.offsetFromArgBase()));
      } else {
        results.getStackResultArea(fr, RegPtr(argLoc.gpr()));
      }
    }
  }

#ifndef RABALDR_PIN_INSTANCE
  fr.loadInstancePtr(InstanceReg);
#endif
  return true;
}

bool BaseCompiler::pushStackResultsForWasmCall(const ResultType& type,
                                               RegPtr temp,
                                               StackResultsLoc* loc) {
  if (!ABIResultIter::HasStackResults(type)) {
    return true;
  }

  // This method can increase stk_.length() by an unbounded amount, so we need
  // to perform an allocation here to accomodate the variable number of values.
  // There is enough headroom for any fixed number of values.  The general case
  // is handled by the operator dispatch.
  if (!stk_.reserve(stk_.length() + type.length())) {
    return false;
  }

  // Measure stack results.
  ABIResultIter i(type);
  size_t count = 0;
  for (; !i.done(); i.next()) {
    if (i.cur().onStack()) {
      count++;
    }
  }
  uint32_t bytes = i.stackBytesConsumedSoFar();

  // Reserve space for the stack results.
  StackHeight resultsBase = fr.stackHeight();
  uint32_t height = fr.prepareStackResultArea(resultsBase, bytes);

  // Push Stk values onto the value stack, and zero out Ref values.
  for (i.switchToPrev(); !i.done(); i.prev()) {
    const ABIResult& result = i.cur();
    if (result.onStack()) {
      Stk v = captureStackResult(result, resultsBase, bytes);
      push(v);
      if (v.kind() == Stk::MemRef) {
        stackMapGenerator_.memRefsOnStk++;
        fr.storeImmediatePtrToStack(intptr_t(0), v.offs(), temp);
      }
    }
  }

  *loc = StackResultsLoc(bytes, count, height);

  return true;
}

// After a call, some results may be written to the stack result locations that
// are pushed on the machine stack after any stack args.  If there are stack
// args and stack results, these results need to be shuffled down, as the args
// are "consumed" by the call.
void BaseCompiler::popStackResultsAfterWasmCall(const StackResultsLoc& results,
                                                uint32_t stackArgBytes) {
  if (results.bytes() != 0) {
    popValueStackBy(results.count());
    if (stackArgBytes != 0) {
      uint32_t srcHeight = results.height();
      MOZ_ASSERT(srcHeight >= stackArgBytes + results.bytes());
      uint32_t destHeight = srcHeight - stackArgBytes;

      fr.shuffleStackResultsTowardFP(srcHeight, destHeight, results.bytes(),
                                     ABINonArgReturnVolatileReg);
    }
  }
}

void BaseCompiler::pushBuiltinCallResult(const FunctionCall& call,
                                         MIRType type) {
  switch (type) {
    case MIRType::Int32: {
      RegI32 rv = captureReturnedI32();
      pushI32(rv);
      break;
    }
    case MIRType::Int64: {
      RegI64 rv = captureReturnedI64();
      pushI64(rv);
      break;
    }
    case MIRType::Float32: {
      RegF32 rv = captureReturnedF32(call);
      pushF32(rv);
      break;
    }
    case MIRType::Double: {
      RegF64 rv = captureReturnedF64(call);
      pushF64(rv);
      break;
    }
#ifdef ENABLE_WASM_SIMD
    case MIRType::Simd128: {
      RegV128 rv = captureReturnedV128(call);
      pushV128(rv);
      break;
    }
#endif
    case MIRType::WasmAnyRef: {
      RegRef rv = captureReturnedRef();
      pushRef(rv);
      break;
    }
    default:
      // In particular, passing |type| as MIRType::Void or MIRType::Pointer to
      // this function is an error.
      MOZ_CRASH("Function return type");
  }
}

bool BaseCompiler::pushWasmCallResults(const FunctionCall& call,
                                       ResultType type,
                                       const StackResultsLoc& loc) {
  // pushResults currently bypasses special case code in captureReturnedFxx()
  // that converts GPR results to FPR results for the system ABI when using
  // softFP.  If we ever start using that combination for calls we need more
  // code.
  MOZ_ASSERT(call.abiKind == ABIKind::Wasm);
  return pushResults(type, fr.stackResultsBase(loc.bytes()));
}

CodeOffset BaseCompiler::callDefinition(uint32_t funcIndex,
                                        const FunctionCall& call) {
  CallSiteDesc desc(bytecodeOffset(), CallSiteKind::Func);
  return masm.call(desc, funcIndex);
}

CodeOffset BaseCompiler::callSymbolic(SymbolicAddress callee,
                                      const FunctionCall& call) {
  CallSiteDesc desc(bytecodeOffset(), CallSiteKind::Symbolic);
  return masm.call(desc, callee);
}

// Precondition: sync()

static ReturnCallAdjustmentInfo BuildReturnCallAdjustmentInfo(
    const FuncType& callerType, const FuncType& calleeType) {
  return ReturnCallAdjustmentInfo(
      StackArgAreaSizeUnaligned(ArgTypeVector(calleeType), ABIKind::Wasm),
      StackArgAreaSizeUnaligned(ArgTypeVector(callerType), ABIKind::Wasm));
}

bool BaseCompiler::callIndirect(uint32_t funcTypeIndex, uint32_t tableIndex,
                                const Stk& indexVal, const FunctionCall& call,
                                bool tailCall, CodeOffset* fastCallOffset,
                                CodeOffset* slowCallOffset) {
  CallIndirectId callIndirectId =
      CallIndirectId::forFuncType(codeMeta_, funcTypeIndex);

  const TableDesc& table = codeMeta_.tables[tableIndex];
  CallSiteDesc desc(bytecodeOffset(), CallSiteKind::Indirect);
  CalleeDesc callee =
      CalleeDesc::wasmTable(codeMeta_, table, tableIndex, callIndirectId);
  StackMap* oobTrapStackMap;
  if (!createDebugOnlyStackMapForNonResumingTrap(&oobTrapStackMap,
                                                 Trap::OutOfBounds)) {
    return false;
  }
  OutOfLineCode* oob = addOutOfLineCode(new (alloc_) OutOfLineTrap(
      Trap::OutOfBounds, trapSiteDesc(), oobTrapStackMap));
  if (!oob) {
    return false;
  }

  if (table.addressType() == AddressType::I64) {
#ifdef JS_PUNBOX64
    RegI64 indexReg = RegI64(Register64(WasmTableCallIndexReg));
#else
    RegI64 indexReg =
        RegI64(Register64(WasmTableCallScratchReg0, WasmTableCallIndexReg));
#endif
    loadI64(indexVal, indexReg);
    masm.branch64(
        Assembler::Condition::BelowOrEqual,
        Address(InstanceReg, wasm::Instance::offsetInData(
                                 callee.tableLengthInstanceDataOffset())),
        indexReg, oob->entry());
    // From this point forward, the callee index is known to fit in 32 bits and
    // therefore we only need WasmTableCallIndexReg.
  } else {
    loadI32(indexVal, RegI32(WasmTableCallIndexReg));
    masm.branch32(
        Assembler::Condition::BelowOrEqual,
        Address(InstanceReg, wasm::Instance::offsetInData(
                                 callee.tableLengthInstanceDataOffset())),
        WasmTableCallIndexReg, oob->entry());
  }

  Label* nullCheckFailed = nullptr;
#ifndef WASM_HAS_HEAPREG
  StackMap* nullTrapStackMap;
  if (!createDebugOnlyStackMapForNonResumingTrap(&nullTrapStackMap,
                                                 Trap::IndirectCallToNull)) {
    return false;
  }
  OutOfLineCode* nullref = addOutOfLineCode(new (alloc_) OutOfLineTrap(
      Trap::IndirectCallToNull, trapSiteDesc(), nullTrapStackMap));
  if (!nullref) {
    return false;
  }
  nullCheckFailed = nullref->entry();
#endif
  if (!tailCall) {
    masm.wasmCallIndirect(desc, callee, nullCheckFailed, fastCallOffset,
                          slowCallOffset);
  } else {
    ReturnCallAdjustmentInfo retCallInfo = BuildReturnCallAdjustmentInfo(
        this->funcType(), (*codeMeta_.types)[funcTypeIndex].funcType());
    masm.wasmReturnCallIndirect(desc, callee, nullCheckFailed, retCallInfo);
  }
  return true;
}

class OutOfLineUpdateCallRefMetrics : public OutOfLineCode {
 public:
  virtual void generate(MacroAssembler* masm, BaseCompiler* bc) override {
    // Call the stub pointed to by Instance::updateCallRefMetricsStub, then
    // rejoin.  See "Register management" in BaseCompiler::updateCallRefMetrics
    // for details of register management.
    //
    // The monitored call may or may not be cross-instance.  The stub will only
    // modify `regMetrics`, `regFuncRef`, `regScratch` and `regMetrics*` (that
    // is, the pointed-at CallRefMetrics) and cannot fail or trap.
    masm->call(
        Address(InstanceReg, Instance::offsetOfUpdateCallRefMetricsStub()));
    masm->jump(rejoin());
  }
};

// Generate code that updates the `callRefIndex`th CallRefMetrics attached to
// the current Instance, to reflect the fact that this call site is just about
// to make a call to the funcref to which WasmCallRefReg currently points.
bool BaseCompiler::updateCallRefMetrics(size_t callRefIndex) {
  AutoCreatedBy acb(masm, "BC::updateCallRefMetrics");

  // See declaration of CallRefMetrics for comments about assignments of
  // funcrefs to `CallRefMetrics::targets[]` fields.

  // Register management: we will use three regs, `regMetrics`, `regFuncRef`,
  // `regScratch` as detailed below.  All of them may be trashed.  But
  // WasmCallRefReg needs to be unchanged across the update, so copy it into
  // `regFuncRef` and use that instead of the original.
  //
  // At entry here, at entry to the OOL code, and at entry to the stub it
  // calls, InstanceReg must be pointing at a valid Instance.
  const Register regMetrics = WasmCallRefCallScratchReg0;  // CallRefMetrics*
  const Register regFuncRef = WasmCallRefCallScratchReg1;  // FuncExtended*
  const Register regScratch = WasmCallRefCallScratchReg2;  // scratch

  OutOfLineCode* ool =
      addOutOfLineCode(new (alloc_) OutOfLineUpdateCallRefMetrics());
  if (!ool) {
    return false;
  }

  // We only need one Rejoin label, so use ool->rejoin() for that.

  // if (target == nullptr) goto Rejoin
  masm.branchWasmAnyRefIsNull(/*isNull=*/true, WasmCallRefReg, ool->rejoin());

  // regFuncRef = target (make a copy of WasmCallRefReg)
  masm.mov(WasmCallRefReg, regFuncRef);

  // regMetrics = thisInstance::callRefMetrics_ + <imm>
  //
  // Emit a patchable mov32 which will load the offset of the `CallRefMetrics`
  // stored inside the `Instance::callRefMetrics_` array
  const CodeOffset offsetOfCallRefOffset = masm.move32WithPatch(regScratch);
  masm.callRefMetricsPatches()[callRefIndex].setOffset(
      offsetOfCallRefOffset.offset());
  // Get a pointer to the `CallRefMetrics` for this call_ref
  masm.loadPtr(Address(InstanceReg, wasm::Instance::offsetOfCallRefMetrics()),
               regMetrics);
  masm.addPtr(regScratch, regMetrics);

  // At this point, regFuncRef = the FuncExtended*, regMetrics = the
  // CallRefMetrics* and we know regFuncRef is not null.
  //
  // if (target->instance != thisInstance) goto Out-Of-Line
  const size_t instanceSlotOffset = FunctionExtended::offsetOfExtendedSlot(
      FunctionExtended::WASM_INSTANCE_SLOT);
  masm.loadPtr(Address(regFuncRef, instanceSlotOffset), regScratch);
  masm.branchPtr(Assembler::NotEqual, InstanceReg, regScratch, ool->entry());

  // At this point, regFuncRef = the FuncExtended*, regMetrics = the
  // CallRefMetrics*, we know regFuncRef is not null and it's a same-instance
  // call.
  //
  // if (target != metrics->targets[0]) goto Out-Of-Line
  const size_t offsetOfTarget0 = CallRefMetrics::offsetOfTarget(0);
  masm.loadPtr(Address(regMetrics, offsetOfTarget0), regScratch);
  masm.branchPtr(Assembler::NotEqual, regScratch, regFuncRef, ool->entry());

  // At this point, regFuncRef = the FuncExtended*, regMetrics = the
  // CallRefMetrics*, we know regFuncRef is not null, it's a same-instance
  // call, and it is to the destination `regMetrics->targets[0]`.
  //
  // metrics->count0++
  const size_t offsetOfCount0 = CallRefMetrics::offsetOfCount(0);
  masm.load32(Address(regMetrics, offsetOfCount0), regScratch);
  masm.add32(Imm32(1), regScratch);
  masm.store32(regScratch, Address(regMetrics, offsetOfCount0));

  masm.bind(ool->rejoin());
  return true;
}

bool BaseCompiler::callRef(const Stk& calleeRef, const FunctionCall& call,
                           mozilla::Maybe<size_t> callRefIndex,
                           CodeOffset* fastCallOffset,
                           CodeOffset* slowCallOffset) {
  CallSiteDesc desc(bytecodeOffset(), CallSiteKind::FuncRef);
  CalleeDesc callee = CalleeDesc::wasmFuncRef();

  loadRef(calleeRef, RegRef(WasmCallRefReg));
  if (compilerEnv_.mode() == CompileMode::LazyTiering) {
    if (!updateCallRefMetrics(*callRefIndex)) {
      return false;
    }
  } else {
    MOZ_ASSERT(callRefIndex.isNothing());
  }

  masm.wasmCallRef(desc, callee, fastCallOffset, slowCallOffset);
  return true;
}

void BaseCompiler::returnCallRef(const Stk& calleeRef, const FunctionCall& call,
                                 const FuncType& funcType) {
  CallSiteDesc desc(bytecodeOffset(), CallSiteKind::FuncRef);
  CalleeDesc callee = CalleeDesc::wasmFuncRef();

  loadRef(calleeRef, RegRef(WasmCallRefReg));
  ReturnCallAdjustmentInfo retCallInfo =
      BuildReturnCallAdjustmentInfo(this->funcType(), funcType);
  masm.wasmReturnCallRef(desc, callee, retCallInfo);
}

// Precondition: sync()

CodeOffset BaseCompiler::callImport(unsigned instanceDataOffset,
                                    const FunctionCall& call) {
  CallSiteDesc desc(bytecodeOffset(), CallSiteKind::Import);
  CalleeDesc callee = CalleeDesc::import(instanceDataOffset);
  return masm.wasmCallImport(desc, callee);
}

CodeOffset BaseCompiler::builtinCall(SymbolicAddress builtin,
                                     const FunctionCall& call) {
  return callSymbolic(builtin, call);
}

void BaseCompiler::builtinInstanceMethodCall(
    const SymbolicAddressSignature& builtin, const ABIArg& instanceArg,
    const FunctionCall& call, CodeOffset* callStackMapKey,
    CodeOffset* trapStackMapKey) {
#ifndef RABALDR_PIN_INSTANCE
  // Builtin method calls assume the instance register has been set.
  fr.loadInstancePtr(InstanceReg);
#endif
  CallSiteDesc desc(bytecodeOffset(), CallSiteKind::Symbolic);
  masm.wasmCallBuiltinInstanceMethod(desc, instanceArg, builtin.identity,
                                     builtin.failureMode, builtin.failureTrap,
                                     callStackMapKey, trapStackMapKey);
}

//////////////////////////////////////////////////////////////////////////////
//
// Exception handling

// Abstracted helper for throwing, used for throw, rethrow, and rethrowing
// at the end of a series of catch blocks (if none matched the exception).
bool BaseCompiler::throwFrom(RegRef exn) {
  pushRef(exn);

  // ThrowException invokes a trap, and the rest is dead code.
  return emitInstanceCall(SASigThrowException);
}

void BaseCompiler::loadTag(RegPtr instance, uint32_t tagIndex, RegRef tagDst) {
  size_t offset =
      Instance::offsetInData(codeMeta_.offsetOfTagInstanceData(tagIndex));
  masm.loadPtr(Address(instance, offset), tagDst);
}

void BaseCompiler::consumePendingException(RegPtr instance, RegRef* exnDst,
                                           RegRef* tagDst) {
  RegPtr pendingAddr = RegPtr(PreBarrierReg);
  needPtr(pendingAddr);
  masm.computeEffectiveAddress(
      Address(instance, Instance::offsetOfPendingException()), pendingAddr);
  *exnDst = needRef();
  masm.loadPtr(Address(pendingAddr, 0), *exnDst);
  emitBarrieredClear(pendingAddr);

  *tagDst = needRef();
  masm.computeEffectiveAddress(
      Address(instance, Instance::offsetOfPendingExceptionTag()), pendingAddr);
  masm.loadPtr(Address(pendingAddr, 0), *tagDst);
  emitBarrieredClear(pendingAddr);
  freePtr(pendingAddr);
}

bool BaseCompiler::startTryNote(size_t* tryNoteIndex) {
  // Check the previous try note to ensure that we don't share an edge with
  // it that could lead to ambiguity. Insert a nop, if required.
  TryNoteVector& tryNotes = masm.tryNotes();
  if (tryNotes.length() > 0) {
    const TryNote& previous = tryNotes.back();
    uint32_t currentOffset = masm.currentOffset();
    if (previous.tryBodyBegin() == currentOffset ||
        previous.tryBodyEnd() == currentOffset) {
      masm.nop();
    }
  }

  // Mark the beginning of the try note
  wasm::TryNote tryNote = wasm::TryNote();
  tryNote.setTryBodyBegin(masm.currentOffset());
  return masm.append(tryNote, tryNoteIndex);
}

void BaseCompiler::finishTryNote(size_t tryNoteIndex) {
  TryNoteVector& tryNotes = masm.tryNotes();
  TryNote& tryNote = tryNotes[tryNoteIndex];

  // Disallow zero-length try notes by inserting a no-op
  if (tryNote.tryBodyBegin() == masm.currentOffset()) {
    masm.nop();
  }

  // Check the most recent finished try note to ensure that we don't share an
  // edge with it that could lead to ambiguity. Insert a nop, if required.
  //
  // Notice that finishTryNote is called in LIFO order -- using depth-first
  // search numbering to see if we are traversing back from a nested try to a
  // parent try, where we may need to ensure that the end offsets do not
  // coincide.
  //
  // In the case the tryNodeIndex >= mostRecentFinishedTryNoteIndex_, we have
  // finished a try that began after the most recent finished try, and so
  // startTryNote will take care of any nops.
  if (tryNoteIndex < mostRecentFinishedTryNoteIndex_) {
    const TryNote& previous = tryNotes[mostRecentFinishedTryNoteIndex_];
    uint32_t currentOffset = masm.currentOffset();
    if (previous.tryBodyEnd() == currentOffset) {
      masm.nop();
    }
  }
  mostRecentFinishedTryNoteIndex_ = tryNoteIndex;

  // Don't set the end of the try note if we've OOM'ed, as the above nop's may
  // not have been placed. This is okay as this compilation will be thrown
  // away.
  if (masm.oom()) {
    return;
  }

  // Mark the end of the try note
  tryNote.setTryBodyEnd(masm.currentOffset());
}

////////////////////////////////////////////////////////////
//
// Platform-specific popping and register targeting.

// The simple popping methods pop values into targeted registers; the caller
// can free registers using standard functions.  These are always called
// popXForY where X says something about types and Y something about the
// operation being targeted.

RegI32 BaseCompiler::needRotate64Temp() {
#if defined(JS_CODEGEN_X86)
  return needI32();
#elif defined(JS_CODEGEN_X64) || defined(JS_CODEGEN_ARM) ||    \
    defined(JS_CODEGEN_ARM64) || defined(JS_CODEGEN_MIPS64) || \
    defined(JS_CODEGEN_LOONG64) || defined(JS_CODEGEN_RISCV64)
  return RegI32::Invalid();
#else
  MOZ_CRASH("BaseCompiler platform hook: needRotate64Temp");
#endif
}

void BaseCompiler::popAndAllocateForDivAndRemI32(RegI32* r0, RegI32* r1,
                                                 RegI32* reserved) {
#if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
  // r0 must be eax, and edx will be clobbered.
  need2xI32(specific_.eax, specific_.edx);
  *r1 = popI32();
  *r0 = popI32ToSpecific(specific_.eax);
  *reserved = specific_.edx;
#else
  pop2xI32(r0, r1);
#endif
}

static void QuotientI32(MacroAssembler& masm, RegI32 rs, RegI32 rsd,
                        RegI32 reserved, IsUnsigned isUnsigned) {
#if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
  masm.quotient32(rsd, rs, rsd, reserved, isUnsigned);
#else
  masm.quotient32(rsd, rs, rsd, isUnsigned);
#endif
}

static void RemainderI32(MacroAssembler& masm, RegI32 rs, RegI32 rsd,
                         RegI32 reserved, IsUnsigned isUnsigned) {
#if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
  masm.remainder32(rsd, rs, rsd, reserved, isUnsigned);
#else
  masm.remainder32(rsd, rs, rsd, isUnsigned);
#endif
}

void BaseCompiler::popAndAllocateForMulI64(RegI64* r0, RegI64* r1,
                                           RegI32* temp) {
#if defined(JS_CODEGEN_X64)
  pop2xI64(r0, r1);
#elif defined(JS_CODEGEN_X86)
  // lhsDest must be edx:eax and rhs must not be that.
  needI64(specific_.edx_eax);
  *r1 = popI64();
  *r0 = popI64ToSpecific(specific_.edx_eax);
  *temp = needI32();
#elif defined(JS_CODEGEN_MIPS64)
  pop2xI64(r0, r1);
#elif defined(JS_CODEGEN_ARM)
  pop2xI64(r0, r1);
  *temp = needI32();
#elif defined(JS_CODEGEN_ARM64)
  pop2xI64(r0, r1);
#elif defined(JS_CODEGEN_LOONG64)
  pop2xI64(r0, r1);
#elif defined(JS_CODEGEN_RISCV64)
  pop2xI64(r0, r1);
#else
  MOZ_CRASH("BaseCompiler porting interface: popAndAllocateForMulI64");
#endif
}

#ifndef RABALDR_INT_DIV_I64_CALLOUT

void BaseCompiler::popAndAllocateForDivAndRemI64(RegI64* r0, RegI64* r1,
                                                 RegI64* reserved,
                                                 IsRemainder isRemainder) {
#  if defined(JS_CODEGEN_X64)
  // r0 must be rax, and rdx will be clobbered.
  need2xI64(specific_.rax, specific_.rdx);
  *r1 = popI64();
  *r0 = popI64ToSpecific(specific_.rax);
  *reserved = specific_.rdx;
#  elif defined(JS_CODEGEN_ARM64)
  pop2xI64(r0, r1);
  if (isRemainder) {
    *reserved = needI64();
  }
#  else
  pop2xI64(r0, r1);
#  endif
}

static void QuotientI64(MacroAssembler& masm, RegI64 rhs, RegI64 srcDest,
                        RegI64 reserved, IsUnsigned isUnsigned) {
#  if defined(JS_CODEGEN_X64)
  // The caller must set up the following situation.
  MOZ_ASSERT(srcDest.reg == rax);
  MOZ_ASSERT(reserved.reg == rdx);
  if (isUnsigned) {
    masm.xorq(rdx, rdx);
    masm.udivq(rhs.reg);
  } else {
    masm.cqo();
    masm.idivq(rhs.reg);
  }
#  elif !defined(JS_CODEGEN_NONE) && !defined(JS_CODEGEN_WASM32)
  masm.quotient64(srcDest.reg, rhs.reg, srcDest.reg, isUnsigned);
#  else
  MOZ_CRASH("BaseCompiler platform hook: quotientI64");
#  endif
}

static void RemainderI64(MacroAssembler& masm, RegI64 rhs, RegI64 srcDest,
                         RegI64 reserved, IsUnsigned isUnsigned) {
#  if defined(JS_CODEGEN_X64)
  // The caller must set up the following situation.
  MOZ_ASSERT(srcDest.reg == rax);
  MOZ_ASSERT(reserved.reg == rdx);

  if (isUnsigned) {
    masm.xorq(rdx, rdx);
    masm.udivq(rhs.reg);
  } else {
    masm.cqo();
    masm.idivq(rhs.reg);
  }
  masm.movq(rdx, rax);
#  elif !defined(JS_CODEGEN_NONE) && !defined(JS_CODEGEN_WASM32)
  masm.remainder64(srcDest.reg, rhs.reg, srcDest.reg, isUnsigned);
#  else
  MOZ_CRASH("BaseCompiler platform hook: remainderI64");
#  endif
}

#endif  // RABALDR_INT_DIV_I64_CALLOUT

RegI32 BaseCompiler::popI32RhsForShift() {
#if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
  // r1 must be ecx for a variable shift, unless BMI2 is available.
  if (!Assembler::HasBMI2()) {
    return popI32(specific_.ecx);
  }
#endif
  RegI32 r = popI32();
#if defined(JS_CODEGEN_ARM)
  masm.and32(Imm32(31), r);
#endif
  return r;
}

RegI32 BaseCompiler::popI32RhsForShiftI64() {
#if defined(JS_CODEGEN_X86)
  // A limitation in the x86 masm requires ecx here
  return popI32(specific_.ecx);
#elif defined(JS_CODEGEN_X64)
  if (!Assembler::HasBMI2()) {
    return popI32(specific_.ecx);
  }
  return popI32();
#else
  return popI32();
#endif
}

RegI64 BaseCompiler::popI64RhsForShift() {
#if defined(JS_CODEGEN_X86)
  // r1 must be ecx for a variable shift.
  needI32(specific_.ecx);
  return popI64ToSpecific(widenI32(specific_.ecx));
#else
#  if defined(JS_CODEGEN_X64)
  // r1 must be rcx for a variable shift, unless BMI2 is available.
  if (!Assembler::HasBMI2()) {
    needI64(specific_.rcx);
    return popI64ToSpecific(specific_.rcx);
  }
#  endif
  // No masking is necessary on 64-bit platforms, and on arm32 the masm
  // implementation masks.
  return popI64();
#endif
}

RegI32 BaseCompiler::popI32RhsForRotate() {
#if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
  // r1 must be ecx for a variable rotate.
  return popI32(specific_.ecx);
#else
  return popI32();
#endif
}

RegI64 BaseCompiler::popI64RhsForRotate() {
#if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
  // r1 must be ecx for a variable rotate.
  needI32(specific_.ecx);
  return popI64ToSpecific(widenI32(specific_.ecx));
#else
  return popI64();
#endif
}

void BaseCompiler::popI32ForSignExtendI64(RegI64* r0) {
#if defined(JS_CODEGEN_X86)
  // r0 must be edx:eax for cdq
  need2xI32(specific_.edx, specific_.eax);
  *r0 = specific_.edx_eax;
  popI32ToSpecific(specific_.eax);
#else
  *r0 = widenI32(popI32());
#endif
}

void BaseCompiler::popI64ForSignExtendI64(RegI64* r0) {
#if defined(JS_CODEGEN_X86)
  // r0 must be edx:eax for cdq
  need2xI32(specific_.edx, specific_.eax);
  // Low on top, high underneath
  *r0 = popI64ToSpecific(specific_.edx_eax);
#else
  *r0 = popI64();
#endif
}

class OutOfLineTruncateCheckF32OrF64ToI32 : public OutOfLineCode {
  AnyReg src;
  RegI32 dest;
  TruncFlags flags;
  TrapSiteDesc trapSiteDesc;

 public:
  OutOfLineTruncateCheckF32OrF64ToI32(AnyReg src, RegI32 dest, TruncFlags flags,
                                      TrapSiteDesc trapSiteDesc)
      : src(src), dest(dest), flags(flags), trapSiteDesc(trapSiteDesc) {}

  virtual void generate(MacroAssembler* masm, BaseCompiler* bc) override {
    if (src.tag == AnyReg::F32) {
      masm->oolWasmTruncateCheckF32ToI32(src.f32(), dest, flags, trapSiteDesc,
                                         rejoin());
    } else if (src.tag == AnyReg::F64) {
      masm->oolWasmTruncateCheckF64ToI32(src.f64(), dest, flags, trapSiteDesc,
                                         rejoin());
    } else {
      MOZ_CRASH("unexpected type");
    }
  }
};

bool BaseCompiler::truncateF32ToI32(RegF32 src, RegI32 dest, TruncFlags flags) {
  OutOfLineCode* ool =
      addOutOfLineCode(new (alloc_) OutOfLineTruncateCheckF32OrF64ToI32(
          AnyReg(src), dest, flags, trapSiteDesc()));
  if (!ool) {
    return false;
  }
  bool isSaturating = flags & TRUNC_SATURATING;
  if (flags & TRUNC_UNSIGNED) {
    masm.wasmTruncateFloat32ToUInt32(src, dest, isSaturating, ool->entry());
  } else {
    masm.wasmTruncateFloat32ToInt32(src, dest, isSaturating, ool->entry());
  }
  masm.bind(ool->rejoin());
  return true;
}

bool BaseCompiler::truncateF64ToI32(RegF64 src, RegI32 dest, TruncFlags flags) {
  OutOfLineCode* ool =
      addOutOfLineCode(new (alloc_) OutOfLineTruncateCheckF32OrF64ToI32(
          AnyReg(src), dest, flags, trapSiteDesc()));
  if (!ool) {
    return false;
  }
  bool isSaturating = flags & TRUNC_SATURATING;
  if (flags & TRUNC_UNSIGNED) {
    masm.wasmTruncateDoubleToUInt32(src, dest, isSaturating, ool->entry());
  } else {
    masm.wasmTruncateDoubleToInt32(src, dest, isSaturating, ool->entry());
  }
  masm.bind(ool->rejoin());
  return true;
}

class OutOfLineTruncateCheckF32OrF64ToI64 : public OutOfLineCode {
  AnyReg src;
  RegI64 dest;
  TruncFlags flags;
  TrapSiteDesc trapSiteDesc;

 public:
  OutOfLineTruncateCheckF32OrF64ToI64(AnyReg src, RegI64 dest, TruncFlags flags,
                                      TrapSiteDesc trapSiteDesc)
      : src(src), dest(dest), flags(flags), trapSiteDesc(trapSiteDesc) {}

  virtual void generate(MacroAssembler* masm, BaseCompiler* bc) override {
    if (src.tag == AnyReg::F32) {
      masm->oolWasmTruncateCheckF32ToI64(src.f32(), dest, flags, trapSiteDesc,
                                         rejoin());
    } else if (src.tag == AnyReg::F64) {
      masm->oolWasmTruncateCheckF64ToI64(src.f64(), dest, flags, trapSiteDesc,
                                         rejoin());
    } else {
      MOZ_CRASH("unexpected type");
    }
  }
};

#ifndef RABALDR_FLOAT_TO_I64_CALLOUT

RegF64 BaseCompiler::needTempForFloatingToI64(TruncFlags flags) {
#  if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
  if (flags & TRUNC_UNSIGNED) {
    return needF64();
  }
#  endif
  return RegF64::Invalid();
}

bool BaseCompiler::truncateF32ToI64(RegF32 src, RegI64 dest, TruncFlags flags,
                                    RegF64 temp) {
  OutOfLineCode* ool =
      addOutOfLineCode(new (alloc_) OutOfLineTruncateCheckF32OrF64ToI64(
          AnyReg(src), dest, flags, trapSiteDesc()));
  if (!ool) {
    return false;
  }
  bool isSaturating = flags & TRUNC_SATURATING;
  if (flags & TRUNC_UNSIGNED) {
    masm.wasmTruncateFloat32ToUInt64(src, dest, isSaturating, ool->entry(),
                                     ool->rejoin(), temp);
  } else {
    masm.wasmTruncateFloat32ToInt64(src, dest, isSaturating, ool->entry(),
                                    ool->rejoin(), temp);
  }
  return true;
}

bool BaseCompiler::truncateF64ToI64(RegF64 src, RegI64 dest, TruncFlags flags,
                                    RegF64 temp) {
  OutOfLineCode* ool =
      addOutOfLineCode(new (alloc_) OutOfLineTruncateCheckF32OrF64ToI64(
          AnyReg(src), dest, flags, trapSiteDesc()));
  if (!ool) {
    return false;
  }
  bool isSaturating = flags & TRUNC_SATURATING;
  if (flags & TRUNC_UNSIGNED) {
    masm.wasmTruncateDoubleToUInt64(src, dest, isSaturating, ool->entry(),
                                    ool->rejoin(), temp);
  } else {
    masm.wasmTruncateDoubleToInt64(src, dest, isSaturating, ool->entry(),
                                   ool->rejoin(), temp);
  }
  return true;
}

#endif  // RABALDR_FLOAT_TO_I64_CALLOUT

#ifndef RABALDR_I64_TO_FLOAT_CALLOUT

RegI32 BaseCompiler::needConvertI64ToFloatTemp(ValType to, bool isUnsigned) {
  bool needs = false;
  if (to == ValType::F64) {
    needs = isUnsigned && masm.convertUInt64ToDoubleNeedsTemp();
  } else {
#  if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
    needs = true;
#  endif
  }
  return needs ? needI32() : RegI32::Invalid();
}

void BaseCompiler::convertI64ToF32(RegI64 src, bool isUnsigned, RegF32 dest,
                                   RegI32 temp) {
  if (isUnsigned) {
    masm.convertUInt64ToFloat32(src, dest, temp);
  } else {
    masm.convertInt64ToFloat32(src, dest);
  }
}

void BaseCompiler::convertI64ToF64(RegI64 src, bool isUnsigned, RegF64 dest,
                                   RegI32 temp) {
  if (isUnsigned) {
    masm.convertUInt64ToDouble(src, dest, temp);
  } else {
    masm.convertInt64ToDouble(src, dest);
  }
}

#endif  // RABALDR_I64_TO_FLOAT_CALLOUT

//////////////////////////////////////////////////////////////////////
//
// Global variable access.

Address BaseCompiler::addressOfGlobalVar(const GlobalDesc& global, RegPtr tmp) {
  uint32_t globalToInstanceOffset = Instance::offsetInData(global.offset());
#ifdef RABALDR_PIN_INSTANCE
  movePtr(RegPtr(InstanceReg), tmp);
#else
  fr.loadInstancePtr(tmp);
#endif
  if (global.isIndirect()) {
    masm.loadPtr(Address(tmp, globalToInstanceOffset), tmp);
    return Address(tmp, 0);
  }
  return Address(tmp, globalToInstanceOffset);
}

//////////////////////////////////////////////////////////////////////
//
// Table access.

Address BaseCompiler::addressOfTableField(uint32_t tableIndex,
                                          uint32_t fieldOffset,
                                          RegPtr instance) {
  uint32_t tableToInstanceOffset = wasm::Instance::offsetInData(
      codeMeta_.offsetOfTableInstanceData(tableIndex) + fieldOffset);
  return Address(instance, tableToInstanceOffset);
}

void BaseCompiler::loadTableLength(uint32_t tableIndex, RegPtr instance,
                                   RegI32 length) {
  masm.load32(addressOfTableField(
                  tableIndex, offsetof(TableInstanceData, length), instance),
              length);
}

void BaseCompiler::loadTableElements(uint32_t tableIndex, RegPtr instance,
                                     RegPtr elements) {
  masm.loadPtr(addressOfTableField(
                   tableIndex, offsetof(TableInstanceData, elements), instance),
               elements);
}

//////////////////////////////////////////////////////////////////////////////
//
// Basic emitters for simple operators.

static void AddI32(MacroAssembler& masm, RegI32 rs, RegI32 rsd) {
  masm.add32(rs, rsd);
}

static void AddImmI32(MacroAssembler& masm, int32_t c, RegI32 rsd) {
  masm.add32(Imm32(c), rsd);
}

static void SubI32(MacroAssembler& masm, RegI32 rs, RegI32 rsd) {
  masm.sub32(rs, rsd);
}

static void SubImmI32(MacroAssembler& masm, int32_t c, RegI32 rsd) {
  masm.sub32(Imm32(c), rsd);
}

static void MulI32(MacroAssembler& masm, RegI32 rs, RegI32 rsd) {
  masm.mul32(rs, rsd);
}

static void OrI32(MacroAssembler& masm, RegI32 rs, RegI32 rsd) {
  masm.or32(rs, rsd);
}

static void OrImmI32(MacroAssembler& masm, int32_t c, RegI32 rsd) {
  masm.or32(Imm32(c), rsd);
}

static void AndI32(MacroAssembler& masm, RegI32 rs, RegI32 rsd) {
  masm.and32(rs, rsd);
}

static void AndImmI32(MacroAssembler& masm, int32_t c, RegI32 rsd) {
  masm.and32(Imm32(c), rsd);
}

static void XorI32(MacroAssembler& masm, RegI32 rs, RegI32 rsd) {
  masm.xor32(rs, rsd);
}

static void XorImmI32(MacroAssembler& masm, int32_t c, RegI32 rsd) {
  masm.xor32(Imm32(c), rsd);
}

static void ClzI32(MacroAssembler& masm, RegI32 rsd) {
  masm.clz32(rsd, rsd, IsKnownNotZero(false));
}

static void CtzI32(MacroAssembler& masm, RegI32 rsd) {
  masm.ctz32(rsd, rsd, IsKnownNotZero(false));
}

// Currently common to PopcntI32 and PopcntI64
static RegI32 PopcntTemp(BaseCompiler& bc) {
#if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
  return AssemblerX86Shared::HasPOPCNT() ? RegI32::Invalid() : bc.needI32();
#elif defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_ARM64) ||    \
    defined(JS_CODEGEN_MIPS64) || defined(JS_CODEGEN_LOONG64) || \
    defined(JS_CODEGEN_RISCV64)
  return bc.needI32();
#else
  MOZ_CRASH("BaseCompiler platform hook: PopcntTemp");
#endif
}

static void PopcntI32(BaseCompiler& bc, RegI32 rsd, RegI32 temp) {
  bc.masm.popcnt32(rsd, rsd, temp);
}

static void ShlI32(MacroAssembler& masm, RegI32 rs, RegI32 rsd) {
  masm.lshift32(rs, rsd);
}

static void ShlImmI32(MacroAssembler& masm, int32_t c, RegI32 rsd) {
  masm.lshift32(Imm32(c & 31), rsd);
}

static void ShrI32(MacroAssembler& masm, RegI32 rs, RegI32 rsd) {
  masm.rshift32Arithmetic(rs, rsd);
}

static void ShrImmI32(MacroAssembler& masm, int32_t c, RegI32 rsd) {
  masm.rshift32Arithmetic(Imm32(c & 31), rsd);
}

static void ShrUI32(MacroAssembler& masm, RegI32 rs, RegI32 rsd) {
  masm.rshift32(rs, rsd);
}

static void ShrUImmI32(MacroAssembler& masm, int32_t c, RegI32 rsd) {
  masm.rshift32(Imm32(c & 31), rsd);
}

static void RotlI32(MacroAssembler& masm, RegI32 rs, RegI32 rsd) {
  masm.rotateLeft(rs, rsd, rsd);
}

static void RotlImmI32(MacroAssembler& masm, int32_t c, RegI32 rsd) {
  masm.rotateLeft(Imm32(c & 31), rsd, rsd);
}

static void RotrI32(MacroAssembler& masm, RegI32 rs, RegI32 rsd) {
  masm.rotateRight(rs, rsd, rsd);
}

static void RotrImmI32(MacroAssembler& masm, int32_t c, RegI32 rsd) {
  masm.rotateRight(Imm32(c & 31), rsd, rsd);
}

static void EqzI32(MacroAssembler& masm, RegI32 rsd) {
  masm.cmp32Set(Assembler::Equal, rsd, Imm32(0), rsd);
}

static void WrapI64ToI32(MacroAssembler& masm, RegI64 rs, RegI32 rd) {
  masm.move64To32(rs, rd);
}

static void AddI64(MacroAssembler& masm, RegI64 rs, RegI64 rsd) {
  masm.add64(rs, rsd);
}

static void AddImmI64(MacroAssembler& masm, int64_t c, RegI64 rsd) {
  masm.add64(Imm64(c), rsd);
}

static void SubI64(MacroAssembler& masm, RegI64 rs, RegI64 rsd) {
  masm.sub64(rs, rsd);
}

static void SubImmI64(MacroAssembler& masm, int64_t c, RegI64 rsd) {
  masm.sub64(Imm64(c), rsd);
}

static void OrI64(MacroAssembler& masm, RegI64 rs, RegI64 rsd) {
  masm.or64(rs, rsd);
}

static void OrImmI64(MacroAssembler& masm, int64_t c, RegI64 rsd) {
  masm.or64(Imm64(c), rsd);
}

static void AndI64(MacroAssembler& masm, RegI64 rs, RegI64 rsd) {
  masm.and64(rs, rsd);
}

static void AndImmI64(MacroAssembler& masm, int64_t c, RegI64 rsd) {
  masm.and64(Imm64(c), rsd);
}

static void XorI64(MacroAssembler& masm, RegI64 rs, RegI64 rsd) {
  masm.xor64(rs, rsd);
}

static void XorImmI64(MacroAssembler& masm, int64_t c, RegI64 rsd) {
  masm.xor64(Imm64(c), rsd);
}

static void ClzI64(BaseCompiler& bc, RegI64 rsd) { bc.masm.clz64(rsd, rsd); }

static void CtzI64(BaseCompiler& bc, RegI64 rsd) { bc.masm.ctz64(rsd, rsd); }

static void PopcntI64(BaseCompiler& bc, RegI64 rsd, RegI32 temp) {
  bc.masm.popcnt64(rsd, rsd, temp);
}

static void ShlI64(BaseCompiler& bc, RegI64 rs, RegI64 rsd) {
  bc.masm.lshift64(bc.lowPart(rs), rsd);
}

static void ShlImmI64(MacroAssembler& masm, int64_t c, RegI64 rsd) {
  masm.lshift64(Imm32(c & 63), rsd);
}

static void ShrI64(BaseCompiler& bc, RegI64 rs, RegI64 rsd) {
  bc.masm.rshift64Arithmetic(bc.lowPart(rs), rsd);
}

static void ShrImmI64(MacroAssembler& masm, int64_t c, RegI64 rsd) {
  masm.rshift64Arithmetic(Imm32(c & 63), rsd);
}

static void ShrUI64(BaseCompiler& bc, RegI64 rs, RegI64 rsd) {
  bc.masm.rshift64(bc.lowPart(rs), rsd);
}

static void ShrUImmI64(MacroAssembler& masm, int64_t c, RegI64 rsd) {
  masm.rshift64(Imm32(c & 63), rsd);
}

static void EqzI64(MacroAssembler& masm, RegI64 rs, RegI32 rd) {
#ifdef JS_PUNBOX64
  masm.cmpPtrSet(Assembler::Equal, rs.reg, ImmWord(0), rd);
#else
  MOZ_ASSERT(rs.low == rd);
  masm.or32(rs.high, rs.low);
  masm.cmp32Set(Assembler::Equal, rs.low, Imm32(0), rd);
#endif
}

static void AddF64(MacroAssembler& masm, RegF64 rs, RegF64 rsd) {
  masm.addDouble(rs, rsd);
}

static void SubF64(MacroAssembler& masm, RegF64 rs, RegF64 rsd) {
  masm.subDouble(rs, rsd);
}

static void MulF64(MacroAssembler& masm, RegF64 rs, RegF64 rsd) {
  masm.mulDouble(rs, rsd);
}

static void DivF64(MacroAssembler& masm, RegF64 rs, RegF64 rsd) {
  masm.divDouble(rs, rsd);
}

static void MinF64(BaseCompiler& bc, RegF64 rs, RegF64 rsd) {
  // Convert signaling NaN to quiet NaNs.
  //
  // TODO / OPTIMIZE (bug 1316824): see comment in MinF32.
#ifdef RABALDR_SCRATCH_F64
  ScratchF64 zero(bc.ra);
#else
  ScratchF64 zero(bc.masm);
#endif
  bc.masm.loadConstantDouble(0, zero);
  bc.masm.subDouble(zero, rsd);
  bc.masm.subDouble(zero, rs);
  bc.masm.minDouble(rs, rsd, HandleNaNSpecially(true));
}

static void MaxF64(BaseCompiler& bc, RegF64 rs, RegF64 rsd) {
  // Convert signaling NaN to quiet NaNs.
  //
  // TODO / OPTIMIZE (bug 1316824): see comment in MinF32.
#ifdef RABALDR_SCRATCH_F64
  ScratchF64 zero(bc.ra);
#else
  ScratchF64 zero(bc.masm);
#endif
  bc.masm.loadConstantDouble(0, zero);
  bc.masm.subDouble(zero, rsd);
  bc.masm.subDouble(zero, rs);
  bc.masm.maxDouble(rs, rsd, HandleNaNSpecially(true));
}

static void CopysignF64(MacroAssembler& masm, RegF64 rs, RegF64 rsd) {
  // No code generated for the no-op case.
  if (rs == rsd) {
    return;
  }
  masm.copySignDouble(rsd, rs, rsd);
}

static void AbsF64(MacroAssembler& masm, RegF64 rsd) {
  masm.absDouble(rsd, rsd);
}

static void NegateF64(MacroAssembler& masm, RegF64 rsd) {
  masm.negateDouble(rsd);
}

static void SqrtF64(MacroAssembler& masm, RegF64 rsd) {
  masm.sqrtDouble(rsd, rsd);
}

static void AddF32(MacroAssembler& masm, RegF32 rs, RegF32 rsd) {
  masm.addFloat32(rs, rsd);
}

static void SubF32(MacroAssembler& masm, RegF32 rs, RegF32 rsd) {
  masm.subFloat32(rs, rsd);
}

static void MulF32(MacroAssembler& masm, RegF32 rs, RegF32 rsd) {
  masm.mulFloat32(rs, rsd);
}

static void DivF32(MacroAssembler& masm, RegF32 rs, RegF32 rsd) {
  masm.divFloat32(rs, rsd);
}

static void MinF32(BaseCompiler& bc, RegF32 rs, RegF32 rsd) {
  // Convert signaling NaN to quiet NaNs.
  //
  // TODO / OPTIMIZE (bug 1316824): Don't do this if one of the operands
  // is known to be a constant.
#ifdef RABALDR_SCRATCH_F32
  ScratchF32 zero(bc.ra);
#else
  ScratchF32 zero(bc.masm);
#endif
  bc.masm.loadConstantFloat32(0.f, zero);
  bc.masm.subFloat32(zero, rsd);
  bc.masm.subFloat32(zero, rs);
  bc.masm.minFloat32(rs, rsd, HandleNaNSpecially(true));
}

static void MaxF32(BaseCompiler& bc, RegF32 rs, RegF32 rsd) {
  // Convert signaling NaN to quiet NaNs.
  //
  // TODO / OPTIMIZE (bug 1316824): see comment in MinF32.
#ifdef RABALDR_SCRATCH_F32
  ScratchF32 zero(bc.ra);
#else
  ScratchF32 zero(bc.masm);
#endif
  bc.masm.loadConstantFloat32(0.f, zero);
  bc.masm.subFloat32(zero, rsd);
  bc.masm.subFloat32(zero, rs);
  bc.masm.maxFloat32(rs, rsd, HandleNaNSpecially(true));
}

static void CopysignF32(MacroAssembler& masm, RegF32 rs, RegF32 rsd) {
  // No code generated for the no-op case.
  if (rs == rsd) {
    return;
  }
  masm.copySignFloat32(rsd, rs, rsd);
}

static void AbsF32(MacroAssembler& masm, RegF32 rsd) {
  masm.absFloat32(rsd, rsd);
}

static void NegateF32(MacroAssembler& masm, RegF32 rsd) {
  masm.negateFloat(rsd);
}

static void SqrtF32(MacroAssembler& masm, RegF32 rsd) {
  masm.sqrtFloat32(rsd, rsd);
}

#ifndef RABALDR_I64_TO_FLOAT_CALLOUT
static void ConvertI64ToF32(MacroAssembler& masm, RegI64 rs, RegF32 rd) {
  masm.convertInt64ToFloat32(rs, rd);
}

static void ConvertI64ToF64(MacroAssembler& masm, RegI64 rs, RegF64 rd) {
  masm.convertInt64ToDouble(rs, rd);
}
#endif

static void ReinterpretF32AsI32(MacroAssembler& masm, RegF32 rs, RegI32 rd) {
  masm.moveFloat32ToGPR(rs, rd);
}

static void ReinterpretF64AsI64(MacroAssembler& masm, RegF64 rs, RegI64 rd) {
  masm.moveDoubleToGPR64(rs, rd);
}

static void ConvertF64ToF32(MacroAssembler& masm, RegF64 rs, RegF32 rd) {
  masm.convertDoubleToFloat32(rs, rd);
}

static void ConvertI32ToF32(MacroAssembler& masm, RegI32 rs, RegF32 rd) {
  masm.convertInt32ToFloat32(rs, rd);
}

static void ConvertU32ToF32(MacroAssembler& masm, RegI32 rs, RegF32 rd) {
  masm.convertUInt32ToFloat32(rs, rd);
}

static void ConvertF32ToF64(MacroAssembler& masm, RegF32 rs, RegF64 rd) {
  masm.convertFloat32ToDouble(rs, rd);
}

static void ConvertI32ToF64(MacroAssembler& masm, RegI32 rs, RegF64 rd) {
  masm.convertInt32ToDouble(rs, rd);
}

static void ConvertU32ToF64(MacroAssembler& masm, RegI32 rs, RegF64 rd) {
  masm.convertUInt32ToDouble(rs, rd);
}

static void ReinterpretI32AsF32(MacroAssembler& masm, RegI32 rs, RegF32 rd) {
  masm.moveGPRToFloat32(rs, rd);
}

static void ReinterpretI64AsF64(MacroAssembler& masm, RegI64 rs, RegF64 rd) {
  masm.moveGPR64ToDouble(rs, rd);
}

static void ExtendI32_8(BaseCompiler& bc, RegI32 rsd) {
#ifdef JS_CODEGEN_X86
  if (!bc.ra.isSingleByteI32(rsd)) {
    ScratchI8 scratch(bc.ra);
    bc.masm.move32(rsd, scratch);
    bc.masm.move8SignExtend(scratch, rsd);
    return;
  }
#endif
  bc.masm.move8SignExtend(rsd, rsd);
}

static void ExtendI32_16(MacroAssembler& masm, RegI32 rsd) {
  masm.move16SignExtend(rsd, rsd);
}

void BaseCompiler::emitMultiplyI64() {
  RegI64 r, rs;
  RegI32 temp;
  popAndAllocateForMulI64(&r, &rs, &temp);
  masm.mul64(rs, r, temp);
  maybeFree(temp);
  freeI64(rs);
  pushI64(r);
}

template <typename RegType, typename IntType>
void BaseCompiler::quotientOrRemainder(
    RegType rs, RegType rsd, RegType reserved, IsUnsigned isUnsigned,
    ZeroOnOverflow zeroOnOverflow, bool isConst, IntType c,
    void (*operate)(MacroAssembler& masm, RegType rs, RegType rsd,
                    RegType reserved, IsUnsigned isUnsigned)) {
  Label done;
  if (!isConst || c == 0) {
    checkDivideByZero(rs);
  }
  if (!isUnsigned && (!isConst || c == -1)) {
    checkDivideSignedOverflow(rs, rsd, &done, zeroOnOverflow);
  }
  operate(masm, rs, rsd, reserved, isUnsigned);
  masm.bind(&done);
}

void BaseCompiler::emitQuotientI32() {
  int32_t c;
  uint_fast8_t power;
  if (popConstPositivePowerOfTwo(&c, &power, 0)) {
    if (power != 0) {
      RegI32 r = popI32();
      Label positive;
      masm.branchTest32(Assembler::NotSigned, r, r, &positive);
      masm.add32(Imm32(c - 1), r);
      masm.bind(&positive);

      masm.rshift32Arithmetic(Imm32(power & 31), r);
      pushI32(r);
    }
  } else {
    bool isConst = peekConst(&c);
    RegI32 r, rs, reserved;
    popAndAllocateForDivAndRemI32(&r, &rs, &reserved);
    quotientOrRemainder(rs, r, reserved, IsUnsigned(false),
                        ZeroOnOverflow(false), isConst, c, QuotientI32);
    maybeFree(reserved);
    freeI32(rs);
    pushI32(r);
  }
}

void BaseCompiler::emitQuotientU32() {
  int32_t c;
  uint_fast8_t power;
  if (popConstPositivePowerOfTwo(&c, &power, 0)) {
    if (power != 0) {
      RegI32 r = popI32();
      masm.rshift32(Imm32(power & 31), r);
      pushI32(r);
    }
  } else {
    bool isConst = peekConst(&c);
    RegI32 r, rs, reserved;
    popAndAllocateForDivAndRemI32(&r, &rs, &reserved);
    quotientOrRemainder(rs, r, reserved, IsUnsigned(true),
                        ZeroOnOverflow(false), isConst, c, QuotientI32);
    maybeFree(reserved);
    freeI32(rs);
    pushI32(r);
  }
}

void BaseCompiler::emitRemainderI32() {
  int32_t c;
  uint_fast8_t power;
  if (popConstPositivePowerOfTwo(&c, &power, 1)) {
    RegI32 r = popI32();
    RegI32 temp = needI32();
    moveI32(r, temp);

    Label positive;
    masm.branchTest32(Assembler::NotSigned, temp, temp, &positive);
    masm.add32(Imm32(c - 1), temp);
    masm.bind(&positive);

    masm.rshift32Arithmetic(Imm32(power & 31), temp);
    masm.lshift32(Imm32(power & 31), temp);
    masm.sub32(temp, r);
    freeI32(temp);

    pushI32(r);
  } else {
    bool isConst = peekConst(&c);
    RegI32 r, rs, reserved;
    popAndAllocateForDivAndRemI32(&r, &rs, &reserved);
    quotientOrRemainder(rs, r, reserved, IsUnsigned(false),
                        ZeroOnOverflow(true), isConst, c, RemainderI32);
    maybeFree(reserved);
    freeI32(rs);
    pushI32(r);
  }
}

void BaseCompiler::emitRemainderU32() {
  int32_t c;
  uint_fast8_t power;
  if (popConstPositivePowerOfTwo(&c, &power, 1)) {
    RegI32 r = popI32();
    masm.and32(Imm32(c - 1), r);
    pushI32(r);
  } else {
    bool isConst = peekConst(&c);
    RegI32 r, rs, reserved;
    popAndAllocateForDivAndRemI32(&r, &rs, &reserved);
    quotientOrRemainder(rs, r, reserved, IsUnsigned(true), ZeroOnOverflow(true),
                        isConst, c, RemainderI32);
    maybeFree(reserved);
    freeI32(rs);
    pushI32(r);
  }
}

#ifndef RABALDR_INT_DIV_I64_CALLOUT
void BaseCompiler::emitQuotientI64() {
  int64_t c;
  uint_fast8_t power;
  if (popConstPositivePowerOfTwo(&c, &power, 0)) {
    if (power != 0) {
      RegI64 r = popI64();
      Label positive;
      masm.branchTest64(Assembler::NotSigned, r, r, &positive);
      masm.add64(Imm64(c - 1), r);
      masm.bind(&positive);

      masm.rshift64Arithmetic(Imm32(power & 63), r);
      pushI64(r);
    }
  } else {
    bool isConst = peekConst(&c);
    RegI64 r, rs, reserved;
    popAndAllocateForDivAndRemI64(&r, &rs, &reserved, IsRemainder(false));
    quotientOrRemainder(rs, r, reserved, IsUnsigned(false),
                        ZeroOnOverflow(false), isConst, c, QuotientI64);
    maybeFree(reserved);
    freeI64(rs);
    pushI64(r);
  }
}

void BaseCompiler::emitQuotientU64() {
  int64_t c;
  uint_fast8_t power;
  if (popConstPositivePowerOfTwo(&c, &power, 0)) {
    if (power != 0) {
      RegI64 r = popI64();
      masm.rshift64(Imm32(power & 63), r);
      pushI64(r);
    }
  } else {
    bool isConst = peekConst(&c);
    RegI64 r, rs, reserved;
    popAndAllocateForDivAndRemI64(&r, &rs, &reserved, IsRemainder(false));
    quotientOrRemainder(rs, r, reserved, IsUnsigned(true),
                        ZeroOnOverflow(false), isConst, c, QuotientI64);
    maybeFree(reserved);
    freeI64(rs);
    pushI64(r);
  }
}

void BaseCompiler::emitRemainderI64() {
  int64_t c;
  uint_fast8_t power;
  if (popConstPositivePowerOfTwo(&c, &power, 1)) {
    RegI64 r = popI64();
    RegI64 temp = needI64();
    moveI64(r, temp);

    Label positive;
    masm.branchTest64(Assembler::NotSigned, temp, temp, &positive);
    masm.add64(Imm64(c - 1), temp);
    masm.bind(&positive);

    masm.rshift64Arithmetic(Imm32(power & 63), temp);
    masm.lshift64(Imm32(power & 63), temp);
    masm.sub64(temp, r);
    freeI64(temp);

    pushI64(r);
  } else {
    bool isConst = peekConst(&c);
    RegI64 r, rs, reserved;
    popAndAllocateForDivAndRemI64(&r, &rs, &reserved, IsRemainder(true));
    quotientOrRemainder(rs, r, reserved, IsUnsigned(false),
                        ZeroOnOverflow(true), isConst, c, RemainderI64);
    maybeFree(reserved);
    freeI64(rs);
    pushI64(r);
  }
}

void BaseCompiler::emitRemainderU64() {
  int64_t c;
  uint_fast8_t power;
  if (popConstPositivePowerOfTwo(&c, &power, 1)) {
    RegI64 r = popI64();
    masm.and64(Imm64(c - 1), r);
    pushI64(r);
  } else {
    bool isConst = peekConst(&c);
    RegI64 r, rs, reserved;
    popAndAllocateForDivAndRemI64(&r, &rs, &reserved, IsRemainder(true));
    quotientOrRemainder(rs, r, reserved, IsUnsigned(true), ZeroOnOverflow(true),
                        isConst, c, RemainderI64);
    maybeFree(reserved);
    freeI64(rs);
    pushI64(r);
  }
}
#endif  // RABALDR_INT_DIV_I64_CALLOUT

void BaseCompiler::emitRotrI64() {
  int64_t c;
  if (popConst(&c)) {
    RegI64 r = popI64();
    RegI32 temp = needRotate64Temp();
    masm.rotateRight64(Imm32(c & 63), r, r, temp);
    maybeFree(temp);
    pushI64(r);
  } else {
    RegI64 rs = popI64RhsForRotate();
    RegI64 r = popI64();
    masm.rotateRight64(lowPart(rs), r, r, maybeHighPart(rs));
    freeI64(rs);
    pushI64(r);
  }
}

void BaseCompiler::emitRotlI64() {
  int64_t c;
  if (popConst(&c)) {
    RegI64 r = popI64();
    RegI32 temp = needRotate64Temp();
    masm.rotateLeft64(Imm32(c & 63), r, r, temp);
    maybeFree(temp);
    pushI64(r);
  } else {
    RegI64 rs = popI64RhsForRotate();
    RegI64 r = popI64();
    masm.rotateLeft64(lowPart(rs), r, r, maybeHighPart(rs));
    freeI64(rs);
    pushI64(r);
  }
}

void BaseCompiler::emitEqzI32() {
  if (sniffConditionalControlEqz(ValType::I32)) {
    return;
  }
  emitUnop(EqzI32);
}

void BaseCompiler::emitEqzI64() {
  if (sniffConditionalControlEqz(ValType::I64)) {
    return;
  }
  emitUnop(EqzI64);
}

template <TruncFlags flags>
bool BaseCompiler::emitTruncateF32ToI32() {
  RegF32 rs = popF32();
  RegI32 rd = needI32();
  if (!truncateF32ToI32(rs, rd, flags)) {
    return false;
  }
  freeF32(rs);
  pushI32(rd);
  return true;
}

template <TruncFlags flags>
bool BaseCompiler::emitTruncateF64ToI32() {
  RegF64 rs = popF64();
  RegI32 rd = needI32();
  if (!truncateF64ToI32(rs, rd, flags)) {
    return false;
  }
  freeF64(rs);
  pushI32(rd);
  return true;
}

#ifndef RABALDR_FLOAT_TO_I64_CALLOUT
template <TruncFlags flags>
bool BaseCompiler::emitTruncateF32ToI64() {
  RegF32 rs = popF32();
  RegI64 rd = needI64();
  RegF64 temp = needTempForFloatingToI64(flags);
  if (!truncateF32ToI64(rs, rd, flags, temp)) {
    return false;
  }
  maybeFree(temp);
  freeF32(rs);
  pushI64(rd);
  return true;
}

template <TruncFlags flags>
bool BaseCompiler::emitTruncateF64ToI64() {
  RegF64 rs = popF64();
  RegI64 rd = needI64();
  RegF64 temp = needTempForFloatingToI64(flags);
  if (!truncateF64ToI64(rs, rd, flags, temp)) {
    return false;
  }
  maybeFree(temp);
  freeF64(rs);
  pushI64(rd);
  return true;
}
#endif  // RABALDR_FLOAT_TO_I64_CALLOUT

void BaseCompiler::emitExtendI64_8() {
  RegI64 r;
  popI64ForSignExtendI64(&r);
  masm.move8To64SignExtend(lowPart(r), r);
  pushI64(r);
}

void BaseCompiler::emitExtendI64_16() {
  RegI64 r;
  popI64ForSignExtendI64(&r);
  masm.move16To64SignExtend(lowPart(r), r);
  pushI64(r);
}

void BaseCompiler::emitExtendI64_32() {
  RegI64 r;
  popI64ForSignExtendI64(&r);
  masm.move32To64SignExtend(lowPart(r), r);
  pushI64(r);
}

void BaseCompiler::emitExtendI32ToI64() {
  RegI64 r;
  popI32ForSignExtendI64(&r);
  masm.move32To64SignExtend(lowPart(r), r);
  pushI64(r);
}

void BaseCompiler::emitExtendU32ToI64() {
  RegI32 rs = popI32();
  RegI64 rd = widenI32(rs);
  masm.move32To64ZeroExtend(rs, rd);
  pushI64(rd);
}

#ifndef RABALDR_I64_TO_FLOAT_CALLOUT
void BaseCompiler::emitConvertU64ToF32() {
  RegI64 rs = popI64();
  RegF32 rd = needF32();
  RegI32 temp = needConvertI64ToFloatTemp(ValType::F32, IsUnsigned(true));
  convertI64ToF32(rs, IsUnsigned(true), rd, temp);
  maybeFree(temp);
  freeI64(rs);
  pushF32(rd);
}

void BaseCompiler::emitConvertU64ToF64() {
  RegI64 rs = popI64();
  RegF64 rd = needF64();
  RegI32 temp = needConvertI64ToFloatTemp(ValType::F64, IsUnsigned(true));
  convertI64ToF64(rs, IsUnsigned(true), rd, temp);
  maybeFree(temp);
  freeI64(rs);
  pushF64(rd);
}
#endif  // RABALDR_I64_TO_FLOAT_CALLOUT

////////////////////////////////////////////////////////////
//
// Machinery for optimized conditional branches.
//
// To disable this optimization it is enough always to return false from
// sniffConditionalControl{Cmp,Eqz}.

struct BranchState {
  union {
    struct {
      RegI32 lhs;
      RegI32 rhs;
      int32_t imm;
      bool rhsImm;
    } i32;
    struct {
      RegI64 lhs;
      RegI64 rhs;
      int64_t imm;
      bool rhsImm;
    } i64;
    struct {
      RegF32 lhs;
      RegF32 rhs;
    } f32;
    struct {
      RegF64 lhs;
      RegF64 rhs;
    } f64;
  };

  Label* const label;             // The target of the branch, never NULL
  const StackHeight stackHeight;  // The stack base above which to place
  // stack-spilled block results, if
  // hasBlockResults().
  const bool invertBranch;      // If true, invert the sense of the branch
  const ResultType resultType;  // The result propagated along the edges

  explicit BranchState(Label* label)
      : label(label),
        stackHeight(StackHeight::Invalid()),
        invertBranch(false),
        resultType(ResultType::Empty()) {}

  BranchState(Label* label, bool invertBranch)
      : label(label),
        stackHeight(StackHeight::Invalid()),
        invertBranch(invertBranch),
        resultType(ResultType::Empty()) {}

  BranchState(Label* label, StackHeight stackHeight, bool invertBranch,
              ResultType resultType)
      : label(label),
        stackHeight(stackHeight),
        invertBranch(invertBranch),
        resultType(resultType) {}

  bool hasBlockResults() const { return stackHeight.isValid(); }
};

void BaseCompiler::setLatentCompare(Assembler::Condition compareOp,
                                    ValType operandType) {
  latentOp_ = LatentOp::Compare;
  latentType_ = operandType;
  latentIntCmp_ = compareOp;
}

void BaseCompiler::setLatentCompare(Assembler::DoubleCondition compareOp,
                                    ValType operandType) {
  latentOp_ = LatentOp::Compare;
  latentType_ = operandType;
  latentDoubleCmp_ = compareOp;
}

void BaseCompiler::setLatentEqz(ValType operandType) {
  latentOp_ = LatentOp::Eqz;
  latentType_ = operandType;
}

bool BaseCompiler::hasLatentOp() const { return latentOp_ != LatentOp::None; }

void BaseCompiler::resetLatentOp() { latentOp_ = LatentOp::None; }

// Emit a conditional branch that optionally and optimally cleans up the CPU
// stack before we branch.
//
// Cond is either Assembler::Condition or Assembler::DoubleCondition.
//
// Lhs is RegI32, RegI64, or RegF32, RegF64, or RegRef.
//
// Rhs is either the same as Lhs, or an immediate expression compatible with
// Lhs "when applicable".

template <typename Cond, typename Lhs, typename Rhs>
bool BaseCompiler::jumpConditionalWithResults(BranchState* b, Cond cond,
                                              Lhs lhs, Rhs rhs) {
  if (b->hasBlockResults()) {
    StackHeight resultsBase(0);
    if (!topBranchParams(b->resultType, &resultsBase)) {
      return false;
    }
    if (b->stackHeight != resultsBase) {
      Label notTaken;
      branchTo(b->invertBranch ? cond : Assembler::InvertCondition(cond), lhs,
               rhs, &notTaken);

      // Shuffle stack args.
      shuffleStackResultsBeforeBranch(resultsBase, b->stackHeight,
                                      b->resultType);
      masm.jump(b->label);
      masm.bind(&notTaken);
      return true;
    }
  }

  branchTo(b->invertBranch ? Assembler::InvertCondition(cond) : cond, lhs, rhs,
           b->label);
  return true;
}

bool BaseCompiler::jumpConditionalWithResults(BranchState* b, RegRef object,
                                              MaybeRefType sourceType,
                                              RefType destType,
                                              bool onSuccess) {
  // Temporarily take the result registers so that branchIfRefSubtype
  // doesn't use them.
  needIntegerResultRegisters(b->resultType);
  BranchIfRefSubtypeRegisters regs =
      allocRegistersForBranchIfRefSubtype(destType);
  freeIntegerResultRegisters(b->resultType);

  if (b->hasBlockResults()) {
    StackHeight resultsBase(0);
    if (!topBranchParams(b->resultType, &resultsBase)) {
      return false;
    }
    if (b->stackHeight != resultsBase) {
      Label notTaken;

      masm.branchWasmRefIsSubtype(
          object, sourceType, destType, &notTaken,
          /*onSuccess=*/b->invertBranch ? onSuccess : !onSuccess,
          /*signalNullChecks=*/false, regs.superSTV, regs.scratch1,
          regs.scratch2);
      freeRegistersForBranchIfRefSubtype(regs);

      // Shuffle stack args.
      shuffleStackResultsBeforeBranch(resultsBase, b->stackHeight,
                                      b->resultType);
      masm.jump(b->label);
      masm.bind(&notTaken);
      return true;
    }
  }

  masm.branchWasmRefIsSubtype(
      object, sourceType, destType, b->label,
      /*onSuccess=*/b->invertBranch ? !onSuccess : onSuccess,
      /*signalNullChecks=*/false, regs.superSTV, regs.scratch1, regs.scratch2);
  freeRegistersForBranchIfRefSubtype(regs);
  return true;
}

// sniffConditionalControl{Cmp,Eqz} may modify the latentWhatever_ state in
// the BaseCompiler so that a subsequent conditional branch can be compiled
// optimally.  emitBranchSetup() and emitBranchPerform() will consume that
// state.  If the latter methods are not called because deadCode_ is true
// then the compiler MUST instead call resetLatentOp() to reset the state.

template <typename Cond>
bool BaseCompiler::sniffConditionalControlCmp(Cond compareOp,
                                              ValType operandType) {
  MOZ_ASSERT(latentOp_ == LatentOp::None,
             "Latent comparison state not properly reset");

#ifdef JS_CODEGEN_X86
  // On x86, latent i64 binary comparisons use too many registers: the
  // reserved join register and the lhs and rhs operands require six, but we
  // only have five.
  if (operandType == ValType::I64) {
    return false;
  }
#endif

  // No optimization for pointer compares yet.
  if (operandType.isRefRepr()) {
    return false;
  }

  // Baseline used to fold a comparison into a following branch or select when
  // it could peek at that operator. itasca reports one operator at a time
  // and no decoder moves, so there is nothing to peek at. Codegen quality
  // only: a driver that held one operator back as a delay line would get the
  // fold back.
  return false;
}

bool BaseCompiler::sniffConditionalControlEqz(ValType operandType) {
  MOZ_ASSERT(latentOp_ == LatentOp::None,
             "Latent comparison state not properly reset");

  // See sniffConditionalControlCmp.
  return false;
}

void BaseCompiler::emitBranchSetup(BranchState* b) {
  // Avoid allocating operands to latentOp_ to result registers.
  if (b->hasBlockResults()) {
    needResultRegisters(b->resultType);
  }

  // Set up fields so that emitBranchPerform() need not switch on latentOp_.
  switch (latentOp_) {
    case LatentOp::None: {
      latentIntCmp_ = Assembler::NotEqual;
      latentType_ = ValType::I32;
      b->i32.lhs = popI32();
      b->i32.rhsImm = true;
      b->i32.imm = 0;
      break;
    }
    case LatentOp::Compare: {
      switch (latentType_.kind()) {
        case ValType::I32: {
          if (popConst(&b->i32.imm)) {
            b->i32.lhs = popI32();
            b->i32.rhsImm = true;
          } else {
            pop2xI32(&b->i32.lhs, &b->i32.rhs);
            b->i32.rhsImm = false;
          }
          break;
        }
        case ValType::I64: {
          pop2xI64(&b->i64.lhs, &b->i64.rhs);
          b->i64.rhsImm = false;
          break;
        }
        case ValType::F32: {
          pop2xF32(&b->f32.lhs, &b->f32.rhs);
          break;
        }
        case ValType::F64: {
          pop2xF64(&b->f64.lhs, &b->f64.rhs);
          break;
        }
        default: {
          MOZ_CRASH("Unexpected type for LatentOp::Compare");
        }
      }
      break;
    }
    case LatentOp::Eqz: {
      switch (latentType_.kind()) {
        case ValType::I32: {
          latentIntCmp_ = Assembler::Equal;
          b->i32.lhs = popI32();
          b->i32.rhsImm = true;
          b->i32.imm = 0;
          break;
        }
        case ValType::I64: {
          latentIntCmp_ = Assembler::Equal;
          b->i64.lhs = popI64();
          b->i64.rhsImm = true;
          b->i64.imm = 0;
          break;
        }
        default: {
          MOZ_CRASH("Unexpected type for LatentOp::Eqz");
        }
      }
      break;
    }
  }

  if (b->hasBlockResults()) {
    freeResultRegisters(b->resultType);
  }
}

bool BaseCompiler::emitBranchPerform(BranchState* b) {
  switch (latentType_.kind()) {
    case ValType::I32: {
      if (b->i32.rhsImm) {
        if (!jumpConditionalWithResults(b, latentIntCmp_, b->i32.lhs,
                                        Imm32(b->i32.imm))) {
          return false;
        }
      } else {
        if (!jumpConditionalWithResults(b, latentIntCmp_, b->i32.lhs,
                                        b->i32.rhs)) {
          return false;
        }
        freeI32(b->i32.rhs);
      }
      freeI32(b->i32.lhs);
      break;
    }
    case ValType::I64: {
      if (b->i64.rhsImm) {
        if (!jumpConditionalWithResults(b, latentIntCmp_, b->i64.lhs,
                                        Imm64(b->i64.imm))) {
          return false;
        }
      } else {
        if (!jumpConditionalWithResults(b, latentIntCmp_, b->i64.lhs,
                                        b->i64.rhs)) {
          return false;
        }
        freeI64(b->i64.rhs);
      }
      freeI64(b->i64.lhs);
      break;
    }
    case ValType::F32: {
      if (!jumpConditionalWithResults(b, latentDoubleCmp_, b->f32.lhs,
                                      b->f32.rhs)) {
        return false;
      }
      freeF32(b->f32.lhs);
      freeF32(b->f32.rhs);
      break;
    }
    case ValType::F64: {
      if (!jumpConditionalWithResults(b, latentDoubleCmp_, b->f64.lhs,
                                      b->f64.rhs)) {
        return false;
      }
      freeF64(b->f64.lhs);
      freeF64(b->f64.rhs);
      break;
    }
    default: {
      MOZ_CRASH("Unexpected type for LatentOp::Compare");
    }
  }
  resetLatentOp();
  return true;
}

// For blocks and loops and ifs:
//
//  - Sync the value stack before going into the block in order to simplify exit
//    from the block: all exits from the block can assume that there are no
//    live registers except the one carrying the exit value.
//  - The block can accumulate a number of dead values on the stacks, so when
//    branching out of the block or falling out at the end be sure to
//    pop the appropriate stacks back to where they were on entry, while
//    preserving the exit value.
//  - A continue branch in a loop is much like an exit branch, but the branch
//    value must not be preserved.
//  - The exit value is always in a designated join register (type dependent).

bool BaseCompiler::emitBlock(BlockType type) {
  if (!pushControl(LabelKind::Block, type)) {
    return false;
  }

  if (!deadCode_) {
    sync();  // Simplifies branching out from block
  }

  initControl(controlItem(), type.params());

  return true;
}

bool BaseCompiler::endBlock(ResultType type) {
  Control& block = controlItem();

  if (deadCode_) {
    // Block does not fall through; reset stack.
    fr.resetStackHeight(block.stackHeight, type);
    popValueStackTo(block.stackSize);
  } else {
    // If the block label is used, we have a control join, so we need to shuffle
    // fallthrough values into place.  Otherwise if it's not a control join, we
    // can leave the value stack alone.
    MOZ_ASSERT(stk_.length() == block.stackSize + type.length());
    if (block.label.used()) {
      popBlockResults(type, block.stackHeight, ContinuationKind::Fallthrough);
    }
    block.bceSafeOnExit &= bceSafe_;
  }

  // Bind after cleanup: branches out will have popped the stack.
  if (block.label.used()) {
    masm.bind(&block.label);
    if (deadCode_) {
      captureResultRegisters(type);
      deadCode_ = false;
    }
    if (!pushBlockResults(type)) {
      return false;
    }
  }

  bceSafe_ = block.bceSafeOnExit;

  return true;
}

bool BaseCompiler::emitLoop(BlockType type) {
  if (!pushControl(LabelKind::Loop, type)) {
    return false;
  }

  if (!deadCode_) {
    sync();  // Simplifies branching out from block
  }

  initControl(controlItem(), type.params());
  bceSafe_ = 0;

  if (!deadCode_) {
    // Loop entry is a control join, so shuffle the entry parameters into the
    // well-known locations.
    if (!topBlockParams(type.params())) {
      return false;
    }
    masm.nopAlign(CodeAlignment);
    masm.bind(&controlItem(0).label);

    // The interrupt check barfs if there are live registers. The hotness check
    // also can call the request tier-up stub, which assumes that no registers
    // are live.
    sync();

    if (!addInterruptCheck()) {
      return false;
    }

    if (compilerEnv_.mode() == CompileMode::LazyTiering) {
      // Create an unpatched hotness check and stash enough information that we
      // can patch it with a value related to the loop's size when we get to
      // the corresponding `end` opcode.
      Maybe<CodeOffset> ctrDecOffset = addHotnessCheck();
      if (ctrDecOffset.isNothing()) {
        return false;
      }
      controlItem().loopBytecodeStart = currentOpOffset_;
      controlItem().offsetOfCtrDec = ctrDecOffset.value();
    }
  }

  return true;
}

// The bodies of the "then" and "else" arms can be arbitrary sequences
// of expressions, they push control and increment the nesting and can
// even be targeted by jumps.  A branch to the "if" block branches to
// the exit of the if, ie, it's like "break".  Consider:
//
//      (func (result i32)
//       (if (i32.const 1)
//           (begin (br 1) (unreachable))
//           (begin (unreachable)))
//       (i32.const 1))
//
// The branch causes neither of the unreachable expressions to be
// evaluated.

bool BaseCompiler::emitIf(BlockType type) {
  if (!pushControl(LabelKind::Then, type)) {
    return false;
  }

  BranchState b(&controlItem().otherLabel, InvertBranch(true));
  if (!deadCode_) {
    needResultRegisters(type.params());
    emitBranchSetup(&b);
    freeResultRegisters(type.params());
    sync();
  } else {
    resetLatentOp();
  }

  initControl(controlItem(), type.params());

  if (!deadCode_) {
    // Because params can flow immediately to results in the case of an empty
    // "then" or "else" block, and the result of an if/then is a join in
    // general, we shuffle params eagerly to the result allocations.
    if (!topBlockParams(type.params())) {
      return false;
    }
    if (!emitBranchPerform(&b)) {
      return false;
    }
  }

  return true;
}

bool BaseCompiler::endIfThen(ResultType type) {
  Control& ifThen = controlItem();

  // The parameters to the "if" logically flow to both the "then" and "else"
  // blocks, but the "else" block is empty.  Since we know that the "if"
  // type-checks, that means that the "else" parameters are the "else" results,
  // and that the "if"'s result type is the same as its parameter type.

  if (deadCode_) {
    // "then" arm does not fall through; reset stack.
    fr.resetStackHeight(ifThen.stackHeight, type);
    popValueStackTo(ifThen.stackSize);
    if (!ifThen.deadOnArrival) {
      captureResultRegisters(type);
    }
  } else {
    MOZ_ASSERT(stk_.length() == ifThen.stackSize + type.length());
    // Assume we have a control join, so place results in block result
    // allocations.
    popBlockResults(type, ifThen.stackHeight, ContinuationKind::Fallthrough);
    MOZ_ASSERT(!ifThen.deadOnArrival);
  }

  if (ifThen.otherLabel.used()) {
    masm.bind(&ifThen.otherLabel);
  }

  if (ifThen.label.used()) {
    masm.bind(&ifThen.label);
  }

  if (!deadCode_) {
    ifThen.bceSafeOnExit &= bceSafe_;
  }

  deadCode_ = ifThen.deadOnArrival;
  if (!deadCode_) {
    if (!pushBlockResults(type)) {
      return false;
    }
  }

  bceSafe_ = ifThen.bceSafeOnExit & ifThen.bceSafeOnEntry;

  return true;
}

bool BaseCompiler::emitElse() {
  BlockType blockType = controlStack_.back().type;
  ResultType params = blockType.params();
  ResultType results = blockType.results();
  controlStack_.back().kind = LabelKind::Else;

  Control& ifThenElse = controlItem(0);

  // See comment in endIfThenElse, below.

  // Exit the "then" branch.

  ifThenElse.deadThenBranch = deadCode_;

  if (deadCode_) {
    fr.resetStackHeight(ifThenElse.stackHeight, results);
    popValueStackTo(ifThenElse.stackSize);
  } else {
    MOZ_ASSERT(stk_.length() == ifThenElse.stackSize + results.length());
    popBlockResults(results, ifThenElse.stackHeight, ContinuationKind::Jump);
    freeResultRegisters(results);
    MOZ_ASSERT(!ifThenElse.deadOnArrival);
  }

  if (!deadCode_) {
    masm.jump(&ifThenElse.label);
  }

  if (ifThenElse.otherLabel.used()) {
    masm.bind(&ifThenElse.otherLabel);
  }

  // Reset to the "else" branch.

  if (!deadCode_) {
    ifThenElse.bceSafeOnExit &= bceSafe_;
  }

  deadCode_ = ifThenElse.deadOnArrival;
  bceSafe_ = ifThenElse.bceSafeOnEntry;

  fr.resetStackHeight(ifThenElse.stackHeight, params);

  if (!deadCode_) {
    captureResultRegisters(params);
    if (!pushBlockResults(params)) {
      return false;
    }
  }

  return true;
}

bool BaseCompiler::endIfThenElse(ResultType type) {
  Control& ifThenElse = controlItem();

  // The expression type is not a reliable guide to what we'll find
  // on the stack, we could have (if E (i32.const 1) (unreachable))
  // in which case the "else" arm is AnyType but the type of the
  // full expression is I32.  So restore whatever's there, not what
  // we want to find there.  The "then" arm has the same constraint.

  if (deadCode_) {
    // "then" arm does not fall through; reset stack.
    fr.resetStackHeight(ifThenElse.stackHeight, type);
    popValueStackTo(ifThenElse.stackSize);
  } else {
    MOZ_ASSERT(stk_.length() == ifThenElse.stackSize + type.length());
    // Assume we have a control join, so place results in block result
    // allocations.
    popBlockResults(type, ifThenElse.stackHeight,
                    ContinuationKind::Fallthrough);
    ifThenElse.bceSafeOnExit &= bceSafe_;
    MOZ_ASSERT(!ifThenElse.deadOnArrival);
  }

  if (ifThenElse.label.used()) {
    masm.bind(&ifThenElse.label);
  }

  bool joinLive =
      !ifThenElse.deadOnArrival &&
      (!ifThenElse.deadThenBranch || !deadCode_ || ifThenElse.label.bound());

  if (joinLive) {
    // No values were provided by the "then" path, but capture the values
    // provided by the "else" path.
    if (deadCode_) {
      captureResultRegisters(type);
    }
    deadCode_ = false;
  }

  bceSafe_ = ifThenElse.bceSafeOnExit;

  if (!deadCode_) {
    if (!pushBlockResults(type)) {
      return false;
    }
  }

  return true;
}

bool BaseCompiler::emitEnd() {
  LabelKind kind = controlStack_.back().kind;
  ResultType type = controlStack_.back().type.results();

  // Every label case is responsible to pop the control item at the appropriate
  // time for the label case
  switch (kind) {
    case LabelKind::Body: {
      if (!endBlock(type)) {
        return false;
      }
      doReturn(ContinuationKind::Fallthrough);
      controlStack_.popBack();
      MOZ_ASSERT(controlStack_.empty());
      // Where the body ended is the validator's to say, so there is no decoder
      // position to check here. The pulled dispatch checks its own in
      // the operator dispatch.
      return true;
    }
    case LabelKind::Block:
      if (!endBlock(type)) {
        return false;
      }
      controlStack_.popBack();
      break;
    case LabelKind::Loop: {
      if (compilerEnv_.mode() == CompileMode::LazyTiering) {
        // These are set (or not set) together.
        MOZ_ASSERT((controlItem().loopBytecodeStart != UINTPTR_MAX) ==
                   (controlItem().offsetOfCtrDec.bound()));
        if (controlItem().loopBytecodeStart != UINTPTR_MAX) {
          // If the above condition is false, the loop was in dead code and so
          // there is no loop-head hotness check that needs to be patched.  See
          // ::emitLoop.
          MOZ_ASSERT(controlItem().loopBytecodeStart <= currentOpOffset_);
          size_t loopBytecodeSize =
              currentOpOffset_ - controlItem().loopBytecodeStart;
          uint32_t step = BlockSizeToDownwardsStep(loopBytecodeSize);
          // Don't try to patch the check if we've OOM'd, since the check might
          // not actually exist.
          if (masm.oom()) {
            return false;
          }
          patchHotnessCheck(controlItem().offsetOfCtrDec, step);
        }
      }
      // The end of a loop isn't a branch target, so we can just leave its
      // results on the expression stack to be consumed by the outer block.
      controlStack_.popBack();
      break;
    }
    case LabelKind::Then:
      if (!endIfThen(type)) {
        return false;
      }
      controlStack_.popBack();
      break;
    case LabelKind::Else:
      if (!endIfThenElse(type)) {
        return false;
      }
      controlStack_.popBack();
      break;
    case LabelKind::Try:
    case LabelKind::Catch:
    case LabelKind::CatchAll:
      if (!endTryCatch(type)) {
        return false;
      }
      controlStack_.popBack();
      break;
    case LabelKind::TryTable:
      if (!endTryTable(type)) {
        return false;
      }
      controlStack_.popBack();
      break;
  }

  return true;
}

bool BaseCompiler::emitBr(uint32_t relativeDepth) {
  ResultType type = branchTargetType(relativeDepth);

  if (deadCode_) {
    return true;
  }

  Control& target = controlItem(relativeDepth);
  target.bceSafeOnExit &= bceSafe_;

  // Save any values in the designated join registers, as if the target block
  // returned normally.

  popBlockResults(type, target.stackHeight, ContinuationKind::Jump);
  masm.jump(&target.label);

  // The registers holding the join values are free for the remainder of this
  // block.

  freeResultRegisters(type);

  deadCode_ = true;

  return true;
}

bool BaseCompiler::emitBrIf(uint32_t relativeDepth) {
  ResultType type = branchTargetType(relativeDepth);

  if (deadCode_) {
    resetLatentOp();
    return true;
  }

  Control& target = controlItem(relativeDepth);
  target.bceSafeOnExit &= bceSafe_;

  BranchState b(&target.label, target.stackHeight, InvertBranch(false), type);
  emitBranchSetup(&b);
  return emitBranchPerform(&b);
}



bool BaseCompiler::emitBrTable(const Uint32Vector& depths,
                              uint32_t defaultDepth) {
  // `branchParams` is the type of the default branch target.  In the presence
  // of subtyping, it could be that the different branch targets have
  // different types.  Here we rely on the assumption that the value
  // representations (e.g. Stk value types) of all branch target types are the
  // same, in the baseline compiler.  Notably, this means that all Ref types
  // should be represented the same.
  ResultType branchParams = branchTargetType(defaultDepth);

  if (deadCode_) {
    return true;
  }

  // Don't use param registers for rc
  needIntegerResultRegisters(branchParams);

  // Table switch value always on top.
  RegI32 rc = popI32();

  freeIntegerResultRegisters(branchParams);

  StackHeight resultsBase(0);
  if (!topBranchParams(branchParams, &resultsBase)) {
    return false;
  }

  Label dispatchCode;
  masm.branch32(Assembler::Below, rc, Imm32(depths.length()), &dispatchCode);

  // This is the out-of-range stub.  rc is dead here but we don't need it.

  shuffleStackResultsBeforeBranch(
      resultsBase, controlItem(defaultDepth).stackHeight, branchParams);
  controlItem(defaultDepth).bceSafeOnExit &= bceSafe_;
  masm.jump(&controlItem(defaultDepth).label);

  // Emit stubs.  rc is dead in all of these but we don't need it.
  //
  // The labels in the vector are in the TempAllocator and will
  // be freed by and by.
  //
  // TODO / OPTIMIZE (Bug 1316804): Branch directly to the case code if we
  // can, don't emit an intermediate stub.

  LabelVector stubs;
  if (!stubs.reserve(depths.length())) {
    return false;
  }

  for (uint32_t depth : depths) {
    stubs.infallibleEmplaceBack(NonAssertingLabel());
    masm.bind(&stubs.back());
    shuffleStackResultsBeforeBranch(resultsBase, controlItem(depth).stackHeight,
                                    branchParams);
    controlItem(depth).bceSafeOnExit &= bceSafe_;
    masm.jump(&controlItem(depth).label);
  }

  // Emit table.

  Label theTable;
  jumpTable(stubs, &theTable);

  // Emit indirect jump.  rc is live here.

  tableSwitch(&theTable, rc, &dispatchCode);

  deadCode_ = true;

  // Clean up.

  freeI32(rc);
  popValueStackBy(branchParams.length());

  return true;
}



void BaseCompiler::emitCatchSetup(LabelKind kind, Control& tryCatch,
                                  const ResultType& resultType) {
  // Catch ends the try or last catch, so we finish this like endIfThen.
  if (deadCode_) {
    fr.resetStackHeight(tryCatch.stackHeight, resultType);
    popValueStackTo(tryCatch.stackSize);
  } else {
    // If the previous block is a catch, we need to handle the extra exception
    // reference on the stack (for rethrow) and thus the stack size is 1 more.
    MOZ_ASSERT(stk_.length() == tryCatch.stackSize + resultType.length() +
                                    (kind == LabelKind::Try ? 0 : 1));
    // Try jumps to the end of the try-catch block unless a throw is done.
    if (kind == LabelKind::Try) {
      popBlockResults(resultType, tryCatch.stackHeight, ContinuationKind::Jump);
    } else {
      popCatchResults(resultType, tryCatch.stackHeight);
    }
    MOZ_ASSERT(stk_.length() == tryCatch.stackSize);
    freeResultRegisters(resultType);
    MOZ_ASSERT(!tryCatch.deadOnArrival);
  }

  // Reset to this "catch" branch.
  deadCode_ = tryCatch.deadOnArrival;

  // We use the empty result type here because catch does *not* take the
  // try-catch block parameters.
  fr.resetStackHeight(tryCatch.stackHeight, ResultType::Empty());

  if (deadCode_) {
    return;
  }

  bceSafe_ = 0;

  // The end of the previous try/catch jumps to the join point.
  masm.jump(&tryCatch.label);

  // Note end of try block for finding the catch block target. This needs
  // to happen after the stack is reset to the correct height.
  if (kind == LabelKind::Try) {
    finishTryNote(controlItem().tryNoteIndex);
  }
}




bool BaseCompiler::endTryCatch(ResultType type) {
  Control& tryCatch = controlItem();
  LabelKind tryKind = controlKind(0);

  if (deadCode_) {
    fr.resetStackHeight(tryCatch.stackHeight, type);
    popValueStackTo(tryCatch.stackSize);
  } else {
    // If the previous block is a catch, we must handle the extra exception
    // reference on the stack (for rethrow) and thus the stack size is 1 more.
    MOZ_ASSERT(stk_.length() == tryCatch.stackSize + type.length() +
                                    (tryKind == LabelKind::Try ? 0 : 1));
    // Assume we have a control join, so place results in block result
    // allocations and also handle the implicit exception reference if needed.
    if (tryKind == LabelKind::Try) {
      popBlockResults(type, tryCatch.stackHeight, ContinuationKind::Jump);
    } else {
      popCatchResults(type, tryCatch.stackHeight);
    }
    MOZ_ASSERT(stk_.length() == tryCatch.stackSize);
    // Since we will emit a landing pad after this and jump over it to get to
    // the control join, we free these here and re-capture at the join.
    freeResultRegisters(type);
    masm.jump(&tryCatch.label);
    MOZ_ASSERT(!tryCatch.bceSafeOnExit);
    MOZ_ASSERT(!tryCatch.deadOnArrival);
  }

  deadCode_ = tryCatch.deadOnArrival;

  if (deadCode_) {
    return true;
  }

  // Create landing pad for all catch handlers in this block.
  // When used for a catchless try block, this will generate a landing pad
  // with no handlers and only the fall-back rethrow.

  // The stack height also needs to be set not for a block result, but for the
  // entry to the exception handlers. This is reset again below for the join.
  StackHeight prePadHeight = fr.stackHeight();
  fr.setStackHeight(tryCatch.stackHeight);

  // If we are in a catchless try block, then there were no catch blocks to
  // mark the end of the try note, so we need to end it here.
  if (tryKind == LabelKind::Try) {
    // Mark the end of the try body. This may insert a nop.
    finishTryNote(controlItem().tryNoteIndex);
  }

  // The landing pad begins at this point
  TryNoteVector& tryNotes = masm.tryNotes();
  TryNote& tryNote = tryNotes[controlItem().tryNoteIndex];
  tryNote.setLandingPad(masm.currentOffset(), masm.framePushed());

  // Store the Instance that was left in InstanceReg by the exception
  // handling mechanism, that is this frame's Instance but with the exception
  // filled in Instance::pendingException.
  fr.storeInstancePtr(InstanceReg);

  // Load exception pointer from Instance and make sure that it is
  // saved before the following call will clear it.
  RegRef exn;
  RegRef tag;
  consumePendingException(RegPtr(InstanceReg), &exn, &tag);

  // Get a register to hold the tags for each catch
  RegRef catchTag = needRef();

  // Ensure that the exception is assigned to the block return register
  // before branching to a handler.
  pushRef(exn);
  ResultType exnResult = ResultType::Single(RefType::extern_());
  popBlockResults(exnResult, tryCatch.stackHeight, ContinuationKind::Jump);
  freeResultRegisters(exnResult);

  bool hasCatchAll = false;
  for (CatchInfo& info : tryCatch.catchInfos) {
    if (info.tagIndex != CatchAllIndex) {
      MOZ_ASSERT(!hasCatchAll);
      loadTag(RegPtr(InstanceReg), info.tagIndex, catchTag);
      masm.branchPtr(Assembler::Equal, tag, catchTag, &info.label);
    } else {
      masm.jump(&info.label);
      hasCatchAll = true;
    }
  }
  freeRef(catchTag);
  freeRef(tag);

  // If none of the tag checks succeed and there is no catch_all,
  // then we rethrow the exception.
  if (!hasCatchAll) {
    captureResultRegisters(exnResult);
    if (!pushBlockResults(exnResult) || !throwFrom(popRef())) {
      return false;
    }
  }

  // Reset stack height for join.
  fr.setStackHeight(prePadHeight);

  // Create join point.
  if (tryCatch.label.used()) {
    masm.bind(&tryCatch.label);
  }

  captureResultRegisters(type);
  deadCode_ = tryCatch.deadOnArrival;
  bceSafe_ = tryCatch.bceSafeOnExit;

  return pushBlockResults(type);
}

bool BaseCompiler::endTryTable(ResultType type) {
  if (!controlItem().deadOnArrival) {
    // Mark the end of the try body. This may insert a nop.
    finishTryNote(controlItem().tryNoteIndex);
  }
  return endBlock(type);
}




bool BaseCompiler::emitDrop() {
  if (deadCode_) {
    return true;
  }

  dropValue();
  return true;
}

void BaseCompiler::doReturn(ContinuationKind kind) {
  if (deadCode_) {
    return;
  }

  StackHeight height = controlOutermost().stackHeight;
  ResultType type = ResultType::Vector(funcType().results());
  popBlockResults(type, height, kind);
  masm.jump(&returnLabel_);
  freeResultRegisters(type);
}

bool BaseCompiler::emitReturn() {
  if (deadCode_) {
    return true;
  }

  doReturn(ContinuationKind::Jump);
  deadCode_ = true;

  return true;
}

// For now, always sync() at the beginning of the call to easily save live
// values.
//
// TODO / OPTIMIZE (Bug 1316806): We may be able to avoid a full sync(), since
// all we want is to save live registers that won't be saved by the callee or
// that we need for outgoing args - we don't need to sync the locals.  We can
// just push the necessary registers, it'll be like a lightweight sync.
//
// Even some of the pushing may be unnecessary if the registers will be consumed
// by the call, because then what we want is parallel assignment to the argument
// registers or onto the stack for outgoing arguments.  A sync() is just
// simpler.

bool BaseCompiler::emitCall(uint32_t funcIndex) {
  if (deadCode_) {
    return true;
  }

  bool import = codeMeta_.funcIsImport(funcIndex);

  if (import) {
    BuiltinModuleFuncId knownFuncImport = codeMeta_.knownFuncImport(funcIndex);
    if (knownFuncImport != BuiltinModuleFuncId::None) {
      const BuiltinModuleFunc& builtinModuleFunc =
          BuiltinModuleFuncs::getFromId(knownFuncImport);
      if (builtinModuleFunc.usesMemory()) {
        // The final parameter of an builtinModuleFunc is implicitly the heap
        // base
        pushHeapBase(0);
      }

      // Call the builtinModuleFunc
      return emitInstanceCall(*builtinModuleFunc.sig());
    }
  }

  sync();

  const FuncType& funcType = codeMeta_.getFuncType(funcIndex);

  uint32_t numArgs = funcType.args().length();
  size_t stackArgBytes = stackConsumed(numArgs);

  ResultType resultType(ResultType::Vector(funcType.results()));
  StackResultsLoc results;
  if (!pushStackResultsForWasmCall(resultType, RegPtr(ABINonArgReg0),
                                   &results)) {
    return false;
  }

  FunctionCall baselineCall(ABIKind::Wasm,
                            import ? RestoreState::All : RestoreState::None);
  beginCall(baselineCall);

  if (!emitCallArgs(funcType.args(), NormalCallResults(results), &baselineCall,
                    CalleeOnStack::False)) {
    return false;
  }

  CodeOffset raOffset;
  if (import) {
    raOffset = callImport(codeMeta_.offsetOfFuncImportInstanceData(funcIndex),
                          baselineCall);
  } else {
    raOffset = callDefinition(funcIndex, baselineCall);
  }

  // stackmap for: emitCall
  if (!createStackMap(Nothing() /* stackmap pertains to a call */, raOffset)) {
    return false;
  }

  popStackResultsAfterWasmCall(results, stackArgBytes);

  endCall(baselineCall, stackArgBytes);

  popValueStackBy(numArgs);

  captureCallResultRegisters(resultType);
  return pushWasmCallResults(baselineCall, resultType, results);
}


bool BaseCompiler::emitCallIndirect(uint32_t funcTypeIndex) {
  // wasm 1.0 has one table, named by a reserved zero byte the validator
  // already checked.
  const uint32_t tableIndex = 0;

  if (deadCode_) {
    return true;
  }

  // Stack: ... arg1 .. argn callee
  sync();

  const FuncType& funcType = (*codeMeta_.types)[funcTypeIndex].funcType();

  uint32_t numArgs = funcType.args().length() + 1;
  size_t stackArgBytes = stackConsumed(numArgs);

  ResultType resultType(ResultType::Vector(funcType.results()));
  StackResultsLoc results;
  if (!pushStackResultsForWasmCall(resultType, RegPtr(ABINonArgReg0),
                                   &results)) {
    return false;
  }

  // State and realm are restored as needed by by callIndirect (really by
  // MacroAssembler::wasmCallIndirect).
  FunctionCall baselineCall(ABIKind::Wasm, RestoreState::None);
  beginCall(baselineCall);

  if (!emitCallArgs(funcType.args(), NormalCallResults(results), &baselineCall,
                    CalleeOnStack::True)) {
    return false;
  }

  const Stk& callee = peek(results.count());
  CodeOffset fastCallOffset;
  CodeOffset slowCallOffset;
  if (!callIndirect(funcTypeIndex, tableIndex, callee, baselineCall,
                    /*tailCall*/ false, &fastCallOffset, &slowCallOffset)) {
    return false;
  }
  // stackmap for: emitCallIndirect (fast)
  if (!createStackMap(Nothing() /* stackmap pertains to a call */,
                      fastCallOffset)) {
    return false;
  }
  // stackmap for: emitCallIndirect (slow)
  if (!createStackMap(Nothing() /* stackmap pertains to a call */,
                      slowCallOffset)) {
    return false;
  }

  popStackResultsAfterWasmCall(results, stackArgBytes);

  endCall(baselineCall, stackArgBytes);

  popValueStackBy(numArgs);

  captureCallResultRegisters(resultType);
  return pushWasmCallResults(baselineCall, resultType, results);
}




void BaseCompiler::emitRound(RoundingMode roundingMode, ValType operandType) {
  if (operandType == ValType::F32) {
    RegF32 f0 = popF32();
    roundF32(roundingMode, f0);
    pushF32(f0);
  } else if (operandType == ValType::F64) {
    RegF64 f0 = popF64();
    roundF64(roundingMode, f0);
    pushF64(f0);
  } else {
    MOZ_CRASH("unexpected type");
  }
}

bool BaseCompiler::emitUnaryMathBuiltinCall(SymbolicAddress callee,
                                            ValType operandType) {
  if (deadCode_) {
    return true;
  }

  RoundingMode roundingMode;
  if (IsRoundingFunction(callee, &roundingMode) &&
      supportsRoundInstruction(roundingMode)) {
    emitRound(roundingMode, operandType);
    return true;
  }

  sync();

  ValTypeVector& signature = operandType == ValType::F32 ? SigF_ : SigD_;
  ValType retType = operandType;
  uint32_t numArgs = signature.length();
  size_t stackSpace = stackConsumed(numArgs);

  FunctionCall baselineCall(ABIForBuiltin(callee), RestoreState::None);
  beginCall(baselineCall);

  if (!emitCallArgs(signature, NoCallResults(), &baselineCall,
                    CalleeOnStack::False)) {
    return false;
  }

  CodeOffset raOffset = builtinCall(callee, baselineCall);
  // stackmap for: emitUnaryMathBuiltinCall
  if (!createStackMap(Nothing() /* stackmap pertains to a call */, raOffset)) {
    return false;
  }

  endCall(baselineCall, stackSpace);

  popValueStackBy(numArgs);

  pushBuiltinCallResult(baselineCall, retType.toMIRType());

  return true;
}

#ifdef RABALDR_INT_DIV_I64_CALLOUT
bool BaseCompiler::emitDivOrModI64BuiltinCall(SymbolicAddress callee,
                                              ValType operandType) {
  MOZ_ASSERT(operandType == ValType::I64);
  MOZ_ASSERT(!deadCode_);

  sync();

  needI64(specific_.abiReturnRegI64);

  RegI64 rhs = popI64();
  RegI64 srcDest = popI64ToSpecific(specific_.abiReturnRegI64);

  Label done;

  checkDivideByZero(rhs);

  if (callee == SymbolicAddress::DivI64) {
    checkDivideSignedOverflow(rhs, srcDest, &done, ZeroOnOverflow(false));
  } else if (callee == SymbolicAddress::ModI64) {
    checkDivideSignedOverflow(rhs, srcDest, &done, ZeroOnOverflow(true));
  }

  masm.setupWasmABICall(callee);
  masm.passABIArg(srcDest.high);
  masm.passABIArg(srcDest.low);
  masm.passABIArg(rhs.high);
  masm.passABIArg(rhs.low);
  CodeOffset raOffset = masm.callWithABI(
      bytecodeOffset(), callee, mozilla::Some(fr.getInstancePtrOffset()));
  // stackmap for: emitDivOrModI64BuiltinCall
  if (!createStackMap(Nothing() /* stackmap pertains to a call */, raOffset)) {
    return false;
  }

  masm.bind(&done);

  freeI64(rhs);
  pushI64(srcDest);
  return true;
}
#endif  // RABALDR_INT_DIV_I64_CALLOUT

#ifdef RABALDR_I64_TO_FLOAT_CALLOUT
bool BaseCompiler::emitConvertInt64ToFloatingCallout(SymbolicAddress callee,
                                                     ValType operandType,
                                                     ValType resultType) {
  sync();

  RegI64 input = popI64();

  FunctionCall call(ABIKind::Wasm, RestoreState::None);

  masm.setupWasmABICall(callee);
#  ifdef JS_PUNBOX64
  MOZ_CRASH("BaseCompiler platform hook: emitConvertInt64ToFloatingCallout");
#  else
  masm.passABIArg(input.high);
  masm.passABIArg(input.low);
#  endif
  CodeOffset raOffset = masm.callWithABI(
      bytecodeOffset(), callee, mozilla::Some(fr.getInstancePtrOffset()),
      resultType == ValType::F32 ? ABIType::Float32 : ABIType::Float64);
  // stackmap for: emitConvertInt64ToFloatingCallout
  if (!createStackMap(Nothing() /* stackmap pertains to a call */, raOffset)) {
    return false;
  }

  freeI64(input);

  if (resultType == ValType::F32) {
    pushF32(captureReturnedF32(call));
  } else {
    pushF64(captureReturnedF64(call));
  }

  return true;
}
#endif  // RABALDR_I64_TO_FLOAT_CALLOUT

#ifdef RABALDR_FLOAT_TO_I64_CALLOUT
// `Callee` always takes a double, so a float32 input must be converted.
bool BaseCompiler::emitConvertFloatingToInt64Callout(SymbolicAddress callee,
                                                     ValType operandType,
                                                     ValType resultType) {
  RegF64 doubleInput;
  if (operandType == ValType::F32) {
    doubleInput = needF64();
    RegF32 input = popF32();
    masm.convertFloat32ToDouble(input, doubleInput);
    freeF32(input);
  } else {
    doubleInput = popF64();
  }

  // We may need the value after the call for the ool check.
  RegF64 otherReg = needF64();
  moveF64(doubleInput, otherReg);
  pushF64(otherReg);

  sync();

  FunctionCall call(ABIKind::Wasm, RestoreState::None);

  masm.setupWasmABICall(callee);
  masm.passABIArg(doubleInput, ABIType::Float64);
  CodeOffset raOffset = masm.callWithABI(
      bytecodeOffset(), callee, mozilla::Some(fr.getInstancePtrOffset()));
  // stackmap for: emitConvertFloatingToInt64Callout
  if (!createStackMap(Nothing() /* stackmap pertains to a call */, raOffset)) {
    return false;
  }

  freeF64(doubleInput);

  RegI64 rv = captureReturnedI64();

  RegF64 inputVal = popF64();

  TruncFlags flags = 0;
  if (callee == SymbolicAddress::TruncateDoubleToUint64) {
    flags |= TRUNC_UNSIGNED;
  }
  if (callee == SymbolicAddress::SaturatingTruncateDoubleToInt64 ||
      callee == SymbolicAddress::SaturatingTruncateDoubleToUint64) {
    flags |= TRUNC_SATURATING;
  }

  // If we're saturating, the callout will always produce the final result
  // value. Otherwise, the callout value will return 0x8000000000000000
  // and we need to produce traps.
  OutOfLineCode* ool = nullptr;
  if (!(flags & TRUNC_SATURATING)) {
    // The OOL check just succeeds or fails, it does not generate a value.
    ool = addOutOfLineCode(new (alloc_) OutOfLineTruncateCheckF32OrF64ToI64(
        AnyReg(inputVal), rv, flags, trapSiteDesc()));
    if (!ool) {
      return false;
    }

    masm.branch64(Assembler::Equal, rv, Imm64(0x8000000000000000),
                  ool->entry());
    masm.bind(ool->rejoin());
  }

  pushI64(rv);
  freeF64(inputVal);

  return true;
}
#endif  // RABALDR_FLOAT_TO_I64_CALLOUT

bool BaseCompiler::emitGetLocal(uint32_t slot) {
  if (deadCode_) {
    return true;
  }

  // Local loads are pushed unresolved, ie, they may be deferred
  // until needed, until they may be affected by a store, or until a
  // sync.  This is intended to reduce register pressure.

  switch ((*locals_)[slot].kind()) {
    case ValType::I32:
      pushLocalI32(slot);
      break;
    case ValType::I64:
      pushLocalI64(slot);
      break;
    case ValType::V128:
#ifdef ENABLE_WASM_SIMD
      pushLocalV128(slot);
      break;
#else
      MOZ_CRASH("No SIMD support");
#endif
    case ValType::F64:
      pushLocalF64(slot);
      break;
    case ValType::F32:
      pushLocalF32(slot);
      break;
    case ValType::Ref:
      pushLocalRef(slot);
      break;
  }

  return true;
}

template <bool isSetLocal>
bool BaseCompiler::emitSetOrTeeLocal(uint32_t slot) {
  if (deadCode_) {
    return true;
  }

  bceLocalIsUpdated(slot);
  switch ((*locals_)[slot].kind()) {
    case ValType::I32: {
      RegI32 rv = popI32();
      syncLocal(slot);
      fr.storeLocalI32(rv, localFromSlot(slot, MIRType::Int32));
      if (isSetLocal) {
        freeI32(rv);
      } else {
        pushI32(rv);
      }
      break;
    }
    case ValType::I64: {
      RegI64 rv = popI64();
      syncLocal(slot);
      fr.storeLocalI64(rv, localFromSlot(slot, MIRType::Int64));
      if (isSetLocal) {
        freeI64(rv);
      } else {
        pushI64(rv);
      }
      break;
    }
    case ValType::F64: {
      RegF64 rv = popF64();
      syncLocal(slot);
      fr.storeLocalF64(rv, localFromSlot(slot, MIRType::Double));
      if (isSetLocal) {
        freeF64(rv);
      } else {
        pushF64(rv);
      }
      break;
    }
    case ValType::F32: {
      RegF32 rv = popF32();
      syncLocal(slot);
      fr.storeLocalF32(rv, localFromSlot(slot, MIRType::Float32));
      if (isSetLocal) {
        freeF32(rv);
      } else {
        pushF32(rv);
      }
      break;
    }
    case ValType::V128: {
#ifdef ENABLE_WASM_SIMD
      RegV128 rv = popV128();
      syncLocal(slot);
      fr.storeLocalV128(rv, localFromSlot(slot, MIRType::Simd128));
      if (isSetLocal) {
        freeV128(rv);
      } else {
        pushV128(rv);
      }
      break;
#else
      MOZ_CRASH("No SIMD support");
#endif
    }
    case ValType::Ref: {
      RegRef rv = popRef();
      syncLocal(slot);
      fr.storeLocalRef(rv, localFromSlot(slot, MIRType::WasmAnyRef));
      if (isSetLocal) {
        freeRef(rv);
      } else {
        pushRef(rv);
      }
      break;
    }
  }

  return true;
}

bool BaseCompiler::emitSetLocal(uint32_t slot) {
  return emitSetOrTeeLocal<true>(slot);
}

bool BaseCompiler::emitTeeLocal(uint32_t slot) {
  return emitSetOrTeeLocal<false>(slot);
}

bool BaseCompiler::emitGetGlobal(uint32_t id) {
  if (deadCode_) {
    return true;
  }

  const GlobalDesc& global = codeMeta_.globals[id];

  if (global.isConstant()) {
    LitVal value = global.constantValue();
    switch (value.type().kind()) {
      case ValType::I32:
        pushI32(value.i32());
        break;
      case ValType::I64:
        pushI64(value.i64());
        break;
      case ValType::F32:
        pushF32(value.f32());
        break;
      case ValType::F64:
        pushF64(value.f64());
        break;
      case ValType::Ref:
        pushRef(intptr_t(value.ref().forCompiledCode()));
        break;
#ifdef ENABLE_WASM_SIMD
      case ValType::V128:
        pushV128(value.v128());
        break;
#endif
      default:
        MOZ_CRASH("Global constant type");
    }
    return true;
  }

  switch (global.type().kind()) {
    case ValType::I32: {
      RegI32 rv = needI32();
      ScratchPtr tmp(*this);
      masm.load32(addressOfGlobalVar(global, tmp), rv);
      pushI32(rv);
      break;
    }
    case ValType::I64: {
      RegI64 rv = needI64();
      ScratchPtr tmp(*this);
      masm.load64(addressOfGlobalVar(global, tmp), rv);
      pushI64(rv);
      break;
    }
    case ValType::F32: {
      RegF32 rv = needF32();
      ScratchPtr tmp(*this);
      masm.loadFloat32(addressOfGlobalVar(global, tmp), rv);
      pushF32(rv);
      break;
    }
    case ValType::F64: {
      RegF64 rv = needF64();
      ScratchPtr tmp(*this);
      masm.loadDouble(addressOfGlobalVar(global, tmp), rv);
      pushF64(rv);
      break;
    }
    case ValType::Ref: {
      RegRef rv = needRef();
      ScratchPtr tmp(*this);
      masm.loadPtr(addressOfGlobalVar(global, tmp), rv);
      pushRef(rv);
      break;
    }
#ifdef ENABLE_WASM_SIMD
    case ValType::V128: {
      RegV128 rv = needV128();
      ScratchPtr tmp(*this);
      masm.loadUnalignedSimd128(addressOfGlobalVar(global, tmp), rv);
      pushV128(rv);
      break;
    }
#endif
    default:
      MOZ_CRASH("Global variable type");
      break;
  }
  return true;
}

bool BaseCompiler::emitSetGlobal(uint32_t id) {
  if (deadCode_) {
    return true;
  }

  const GlobalDesc& global = codeMeta_.globals[id];

  switch (global.type().kind()) {
    case ValType::I32: {
      RegI32 rv = popI32();
      ScratchPtr tmp(*this);
      masm.store32(rv, addressOfGlobalVar(global, tmp));
      freeI32(rv);
      break;
    }
    case ValType::I64: {
      RegI64 rv = popI64();
      ScratchPtr tmp(*this);
      masm.store64(rv, addressOfGlobalVar(global, tmp));
      freeI64(rv);
      break;
    }
    case ValType::F32: {
      RegF32 rv = popF32();
      ScratchPtr tmp(*this);
      masm.storeFloat32(rv, addressOfGlobalVar(global, tmp));
      freeF32(rv);
      break;
    }
    case ValType::F64: {
      RegF64 rv = popF64();
      ScratchPtr tmp(*this);
      masm.storeDouble(rv, addressOfGlobalVar(global, tmp));
      freeF64(rv);
      break;
    }
    case ValType::Ref: {
      RegPtr valueAddr(PreBarrierReg);
      needPtr(valueAddr);
      {
        ScratchPtr tmp(*this);
        masm.computeEffectiveAddress(addressOfGlobalVar(global, tmp),
                                     valueAddr);
      }
      RegRef rv = popRef();
      // emitBarrieredStore preserves rv
      if (!emitBarrieredStore(Nothing(), valueAddr, rv, PreBarrierKind::Normal,
                              PostBarrierKind::Imprecise)) {
        return false;
      }
      freeRef(rv);
      break;
    }
#ifdef ENABLE_WASM_SIMD
    case ValType::V128: {
      RegV128 rv = popV128();
      ScratchPtr tmp(*this);
      masm.storeUnalignedSimd128(rv, addressOfGlobalVar(global, tmp));
      freeV128(rv);
      break;
    }
#endif
    default:
      MOZ_CRASH("Global variable type");
      break;
  }
  return true;
}

bool BaseCompiler::emitLoad(ValType type, Scalar::Type viewType,
                            uint32_t alignLog2, uint32_t offset) {
  if (deadCode_) {
    return true;
  }
  // wasm 1.0 has one memory and no memory index in the memarg.
  MemoryAccessDesc access(/*memoryIndex=*/0, viewType,
                          uint32_t(1) << alignLog2, offset, trapSiteDesc(),
                          hugeMemoryEnabled(/*memoryIndex=*/0));
  loadCommon(&access, AccessCheck(), type);
  return true;
}

bool BaseCompiler::emitStore(ValType resultType, Scalar::Type viewType,
                             uint32_t alignLog2, uint32_t offset) {
  if (deadCode_) {
    return true;
  }
  // wasm 1.0 has one memory and no memory index in the memarg.
  MemoryAccessDesc access(/*memoryIndex=*/0, viewType,
                          uint32_t(1) << alignLog2, offset, trapSiteDesc(),
                          hugeMemoryEnabled(/*memoryIndex=*/0));
  storeCommon(&access, AccessCheck(), resultType);
  return true;
}

bool BaseCompiler::emitSelect(VwStackType vwType) {
  if (deadCode_) {
    resetLatentOp();
    return true;
  }

  // Only converted once dead code is ruled out: a bottom type is
  // unconstrained and only occurs in unreachable code.
  ValType type = ToWasmValType(vwType);

  // I32 condition on top, then false, then true.

  Label done;
  BranchState b(&done);
  emitBranchSetup(&b);

  switch (type.kind()) {
    case ValType::I32: {
      RegI32 r, rs;
      pop2xI32(&r, &rs);
      if (!emitBranchPerform(&b)) {
        return false;
      }
      moveI32(rs, r);
      masm.bind(&done);
      freeI32(rs);
      pushI32(r);
      break;
    }
    case ValType::I64: {
#ifdef JS_CODEGEN_X86
      // There may be as many as four Int64 values in registers at a time: two
      // for the latent branch operands, and two for the true/false values we
      // normally pop before executing the branch.  On x86 this is one value
      // too many, so we need to generate more complicated code here, and for
      // simplicity's sake we do so even if the branch operands are not Int64.
      // However, the resulting control flow diamond is complicated since the
      // arms of the diamond will have to stay synchronized with respect to
      // their evaluation stack and regalloc state.  To simplify further, we
      // use a double branch and a temporary boolean value for now.
      RegI32 temp = needI32();
      moveImm32(0, temp);
      if (!emitBranchPerform(&b)) {
        return false;
      }
      moveImm32(1, temp);
      masm.bind(&done);

      Label trueValue;
      RegI64 r, rs;
      pop2xI64(&r, &rs);
      masm.branch32(Assembler::Equal, temp, Imm32(0), &trueValue);
      moveI64(rs, r);
      masm.bind(&trueValue);
      freeI32(temp);
      freeI64(rs);
      pushI64(r);
#else
      RegI64 r, rs;
      pop2xI64(&r, &rs);
      if (!emitBranchPerform(&b)) {
        return false;
      }
      moveI64(rs, r);
      masm.bind(&done);
      freeI64(rs);
      pushI64(r);
#endif
      break;
    }
    case ValType::F32: {
      RegF32 r, rs;
      pop2xF32(&r, &rs);
      if (!emitBranchPerform(&b)) {
        return false;
      }
      moveF32(rs, r);
      masm.bind(&done);
      freeF32(rs);
      pushF32(r);
      break;
    }
    case ValType::F64: {
      RegF64 r, rs;
      pop2xF64(&r, &rs);
      if (!emitBranchPerform(&b)) {
        return false;
      }
      moveF64(rs, r);
      masm.bind(&done);
      freeF64(rs);
      pushF64(r);
      break;
    }
#ifdef ENABLE_WASM_SIMD
    case ValType::V128: {
      RegV128 r, rs;
      pop2xV128(&r, &rs);
      if (!emitBranchPerform(&b)) {
        return false;
      }
      moveV128(rs, r);
      masm.bind(&done);
      freeV128(rs);
      pushV128(r);
      break;
    }
#endif
    case ValType::Ref: {
      RegRef r, rs;
      pop2xRef(&r, &rs);
      if (!emitBranchPerform(&b)) {
        return false;
      }
      moveRef(rs, r);
      masm.bind(&done);
      freeRef(rs);
      pushRef(r);
      break;
    }
    default: {
      MOZ_CRASH("select type");
    }
  }

  return true;
}

void BaseCompiler::emitCompareI32(Assembler::Condition compareOp,
                                  ValType compareType) {
  MOZ_ASSERT(compareType == ValType::I32);

  if (sniffConditionalControlCmp(compareOp, compareType)) {
    return;
  }

  int32_t c;
  if (popConst(&c)) {
    RegI32 r = popI32();
    masm.cmp32Set(compareOp, r, Imm32(c), r);
    pushI32(r);
  } else {
    RegI32 r, rs;
    pop2xI32(&r, &rs);
    masm.cmp32Set(compareOp, r, rs, r);
    freeI32(rs);
    pushI32(r);
  }
}

void BaseCompiler::emitCompareI64(Assembler::Condition compareOp,
                                  ValType compareType) {
  MOZ_ASSERT(compareType == ValType::I64);

  if (sniffConditionalControlCmp(compareOp, compareType)) {
    return;
  }

  RegI64 rs0, rs1;
  pop2xI64(&rs0, &rs1);
  RegI32 rd(fromI64(rs0));
  cmp64Set(compareOp, rs0, rs1, rd);
  freeI64(rs1);
  freeI64Except(rs0, rd);
  pushI32(rd);
}

void BaseCompiler::emitCompareF32(Assembler::DoubleCondition compareOp,
                                  ValType compareType) {
  MOZ_ASSERT(compareType == ValType::F32);

  if (sniffConditionalControlCmp(compareOp, compareType)) {
    return;
  }

  Label across;
  RegF32 rs0, rs1;
  pop2xF32(&rs0, &rs1);
  RegI32 rd = needI32();
  moveImm32(1, rd);
  masm.branchFloat(compareOp, rs0, rs1, &across);
  moveImm32(0, rd);
  masm.bind(&across);
  freeF32(rs0);
  freeF32(rs1);
  pushI32(rd);
}

void BaseCompiler::emitCompareF64(Assembler::DoubleCondition compareOp,
                                  ValType compareType) {
  MOZ_ASSERT(compareType == ValType::F64);

  if (sniffConditionalControlCmp(compareOp, compareType)) {
    return;
  }

  Label across;
  RegF64 rs0, rs1;
  pop2xF64(&rs0, &rs1);
  RegI32 rd = needI32();
  moveImm32(1, rd);
  masm.branchDouble(compareOp, rs0, rs1, &across);
  moveImm32(0, rd);
  masm.bind(&across);
  freeF64(rs0);
  freeF64(rs1);
  pushI32(rd);
}

void BaseCompiler::emitCompareRef(Assembler::Condition compareOp,
                                  ValType compareType) {
  MOZ_ASSERT(!sniffConditionalControlCmp(compareOp, compareType));

  RegRef rs1, rs2;
  pop2xRef(&rs1, &rs2);
  RegI32 rd = needI32();
  masm.cmpPtrSet(compareOp, rs1, rs2, rd);
  freeRef(rs1);
  freeRef(rs2);
  pushI32(rd);
}

bool BaseCompiler::emitInstanceCall(const SymbolicAddressSignature& builtin) {
  // See declaration (WasmBCClass.h) for info on the relationship between the
  // compiler's value stack and the argument order for the to-be-called
  // function.
  const MIRType* argTypes = builtin.argTypes;
  MOZ_ASSERT(argTypes[0] == MIRType::Pointer);

  sync();

  uint32_t numNonInstanceArgs = builtin.numArgs - 1 /* instance */;
  size_t stackSpace = stackConsumed(numNonInstanceArgs);

  FunctionCall baselineCall(ABIForBuiltin(builtin.identity),
                            RestoreState::PinnedRegs);
  beginCall(baselineCall);

  ABIArg instanceArg = reservePointerArgument(&baselineCall);

  startCallArgs(StackArgAreaSizeUnaligned(builtin, baselineCall.abiKind),
                &baselineCall);
  for (uint32_t i = 1; i < builtin.numArgs; i++) {
    ValType t;
    switch (argTypes[i]) {
      case MIRType::Int32:
        t = ValType::I32;
        break;
      case MIRType::Int64:
        t = ValType::I64;
        break;
      case MIRType::Float32:
        t = ValType::F32;
        break;
      case MIRType::WasmAnyRef:
        t = RefType::extern_();
        break;
      case MIRType::Pointer:
        // Instance function args can now be uninterpreted pointers (eg, for
        // the cases PostBarrier and PostBarrierFilter) so we simply treat
        // them like the equivalently sized integer.
        t = ValType::fromMIRType(TargetWordMIRType());
        break;
      default:
        MOZ_CRASH("Unexpected type");
    }
    passArg(t, peek(numNonInstanceArgs - i), &baselineCall);
  }

  CodeOffset callStackMapKey;
  CodeOffset trapStackMapKey;
  builtinInstanceMethodCall(builtin, instanceArg, baselineCall,
                            &callStackMapKey, &trapStackMapKey);
  if (!createStackMap(Nothing() /* stackmap pertains to a call */,
                      callStackMapKey)) {
    return false;
  }
  if (trapStackMapKey.bound() &&
      // FIXME: this is a kludge in that it assumes that the trap kind created
      // by builtinInstanceMethodCall is ThrowReported.
      !createStackMap(Some(wasm::Trap::ThrowReported), trapStackMapKey)) {
    return false;
  }
  endCall(baselineCall, stackSpace);

  popValueStackBy(numNonInstanceArgs);

  // Note, many clients of emitInstanceCall currently assume that pushing the
  // result here does not destroy ReturnReg.
  //
  // Furthermore, clients assume that if builtin.retType != MIRType::None, the
  // callee will have returned a result and left it in ReturnReg for us to
  // find, and that that register will not be destroyed here (or above).

  // For the return type only, MIRType::None is used to indicate that the
  // call doesn't return a result, that is, returns a C/C++ "void".

  if (builtin.retType != MIRType::None) {
    pushBuiltinCallResult(baselineCall, builtin.retType);
  }
  return true;
}













//////////////////////////////////////////////////////////////////////////////
//
// Bulk memory operations.

bool BaseCompiler::emitMemoryGrow() {
  // wasm 1.0 has one memory, named by a reserved zero byte the validator
  // already checked.
  const uint32_t memoryIndex = 0;
  if (deadCode_) {
    return true;
  }

  pushI32(memoryIndex);
  return emitInstanceCall(isMem32(memoryIndex) ? SASigMemoryGrowM32
                                               : SASigMemoryGrowM64);
}

bool BaseCompiler::emitMemorySize() {
  const uint32_t memoryIndex = 0;
  if (deadCode_) {
    return true;
  }

  pushI32(memoryIndex);
  return emitInstanceCall(isMem32(memoryIndex) ? SASigMemorySizeM32
                                               : SASigMemorySizeM64);
}


bool BaseCompiler::memCopyCall(uint32_t dstMemIndex, uint32_t srcMemIndex) {
  // Common and optimized path for when the src/dest memories are the same
  if (dstMemIndex == srcMemIndex) {
    bool mem32 = isMem32(dstMemIndex);
    pushHeapBase(dstMemIndex);
    return emitInstanceCall(
        usesSharedMemory(dstMemIndex)
            ? (mem32 ? SASigMemCopySharedM32 : SASigMemCopySharedM64)
            : (mem32 ? SASigMemCopyM32 : SASigMemCopyM64));
  }

  // Do the general-purpose fallback for copying between any combination of
  // memories. This works by moving everything to the lowest-common denominator.
  // i32 addresses are promoted to i64, and non-shared memories are treated as
  // shared.
  AddressType dstAddressType = codeMeta_.memories[dstMemIndex].addressType();
  AddressType srcAddressType = codeMeta_.memories[srcMemIndex].addressType();
  AddressType lenAddressType = MinAddressType(dstAddressType, srcAddressType);

  // Pop the operands off of the stack and widen them
  RegI64 len = popAddressToInt64(lenAddressType);
#ifdef JS_CODEGEN_X86
  {
    // Stash the length value to prevent running out of registers
    ScratchPtr scratch(*this);
    stashI64(scratch, len);
    freeI64(len);
  }
#endif
  RegI64 srcIndex = popAddressToInt64(srcAddressType);
  RegI64 dstIndex = popAddressToInt64(dstAddressType);

  pushI64(dstIndex);
  pushI64(srcIndex);
#ifdef JS_CODEGEN_X86
  {
    // Unstash the length value and push it back on the stack
    ScratchPtr scratch(*this);
    len = needI64();
    unstashI64(scratch, len);
  }
#endif
  pushI64(len);
  pushI32(dstMemIndex);
  pushI32(srcMemIndex);
  return emitInstanceCall(SASigMemCopyAny);
}


bool BaseCompiler::memFillCall(uint32_t memoryIndex) {
  pushHeapBase(memoryIndex);
  return emitInstanceCall(
      usesSharedMemory(memoryIndex)
          ? (isMem32(memoryIndex) ? SASigMemFillSharedM32
                                  : SASigMemFillSharedM64)
          : (isMem32(memoryIndex) ? SASigMemFillM32 : SASigMemFillM64));
}










void BaseCompiler::emitTableBoundsCheck(uint32_t tableIndex, RegI32 address,
                                        RegPtr instance) {
  Label ok;
  masm.wasmBoundsCheck32(
      Assembler::Condition::Below, address,
      addressOfTableField(tableIndex, offsetof(TableInstanceData, length),
                          instance),
      &ok);
  trap(wasm::Trap::OutOfBounds);
  masm.bind(&ok);
}

bool BaseCompiler::emitTableGetAnyRef(uint32_t tableIndex) {
  RegPtr instance = needPtr();
  RegPtr elements = needPtr();
  RegI32 address = popI32();

  fr.loadInstancePtr(instance);
  emitTableBoundsCheck(tableIndex, address, instance);
  loadTableElements(tableIndex, instance, elements);
  masm.loadPtr(BaseIndex(elements, address, ScalePointer), elements);

  pushRef(RegRef(elements));
  freeI32(address);
  freePtr(instance);

  return true;
}

bool BaseCompiler::emitTableSetAnyRef(uint32_t tableIndex) {
  // Create temporaries for valueAddr that is not in the prebarrier register
  // and can be consumed by the barrier operation
  RegPtr valueAddr = RegPtr(PreBarrierReg);
  needPtr(valueAddr);

  RegPtr instance = needPtr();
  RegPtr elements = needPtr();
  RegRef value = popRef();
  RegI32 address = popI32();

  // x86 is one register too short for this operation, shuffle `value` back
  // onto the stack until it is needed.
#ifdef JS_CODEGEN_X86
  pushRef(value);
#endif

  fr.loadInstancePtr(instance);
  emitTableBoundsCheck(tableIndex, address, instance);
  loadTableElements(tableIndex, instance, elements);
  masm.computeEffectiveAddress(BaseIndex(elements, address, ScalePointer),
                               valueAddr);

  freeI32(address);
  freePtr(elements);
  freePtr(instance);

#ifdef JS_CODEGEN_X86
  value = popRef();
#endif

  if (!emitBarrieredStore(Nothing(), valueAddr, value, PreBarrierKind::Normal,
                          PostBarrierKind::Precise)) {
    return false;
  }
  freeRef(value);
  return true;
}




//////////////////////////////////////////////////////////////////////////////
//
// General object support.

void BaseCompiler::emitPreBarrier(RegPtr valueAddr) {
  Label skipBarrier;
  ScratchPtr scratch(*this);

#ifdef RABALDR_PIN_INSTANCE
  Register instance(InstanceReg);
#else
  Register instance(scratch);
  fr.loadInstancePtr(instance);
#endif

  EmitWasmPreBarrierGuard(masm, instance, scratch, Address(valueAddr, 0),
                          &skipBarrier, mozilla::Nothing());

#ifndef RABALDR_PIN_INSTANCE
  fr.loadInstancePtr(instance);
#endif
#ifdef JS_CODEGEN_ARM64
  // The prebarrier stub assumes the PseudoStackPointer is set up.  It is OK
  // to just move the sp to x20 here because x20 is not being used by the
  // baseline compiler and need not be saved or restored.
  MOZ_ASSERT(!GeneralRegisterSet::All().hasRegisterIndex(x20.asUnsized()));
  masm.Mov(x20, sp);
#endif
  // The prebarrier call preserves all volatile registers
  EmitWasmPreBarrierCallImmediate(masm, instance, scratch, valueAddr,
                                  /*valueOffset=*/0);

  masm.bind(&skipBarrier);
}

bool BaseCompiler::emitPostBarrierWholeCell(RegRef object, RegRef value,
                                            RegPtr temp) {
  // We must force a sync before the guard so that locals are in a consistent
  // location for whether or not the post-barrier call is taken.
  sync();

  // Emit guards to skip the post-barrier call if it is not needed.
  Label skipBarrier;
  EmitWasmPostBarrierGuard(masm, mozilla::Some(object), temp, value,
                           &skipBarrier);

#ifdef RABALDR_PIN_INSTANCE
  Register instance(InstanceReg);
#else
  Register instance(temp);
  fr.loadInstancePtr(instance);
#endif
  CheckWholeCellLastElementCache(masm, instance, object, temp, &skipBarrier);

  movePtr(RegPtr(object), temp);

  // Push `object` and `value` to preserve them across the call.
  pushRef(object);
  pushRef(value);

  pushPtr(temp);
  if (!emitInstanceCall(SASigPostBarrierWholeCell)) {
    return false;
  }

  // Restore `object` and `value`.
  popRef(value);
  popRef(object);

  masm.bind(&skipBarrier);
  return true;
}

bool BaseCompiler::emitPostBarrierEdgeImprecise(const Maybe<RegRef>& object,
                                                RegPtr valueAddr,
                                                RegRef value) {
  // We must force a sync before the guard so that locals are in a consistent
  // location for whether or not the post-barrier call is taken.
  sync();

  // Emit a guard to skip the post-barrier call if it is not needed.
  Label skipBarrier;
  RegPtr otherScratch = needPtr();
  EmitWasmPostBarrierGuard(masm, object, otherScratch, value, &skipBarrier);
  freePtr(otherScratch);

  // Push `object` and `value` to preserve them across the call.
  if (object) {
    pushRef(*object);
  }
  pushRef(value);

  // The `valueAddr` is a raw pointer to the cell within some GC object or
  // instance area, and we are careful so that the GC will not run while the
  // post-barrier call is active, so push a uintptr_t value.
  pushPtr(valueAddr);
  if (!emitInstanceCall(SASigPostBarrierEdge)) {
    return false;
  }

  // Restore `object` and `value`.
  popRef(value);
  if (object) {
    popRef(*object);
  }

  masm.bind(&skipBarrier);
  return true;
}

bool BaseCompiler::emitPostBarrierEdgePrecise(const Maybe<RegRef>& object,
                                              RegPtr valueAddr,
                                              RegRef prevValue, RegRef value) {
  // Currently this is only called to write into wasm tables.
  //
  // If this changes and we use this method to write into objects which might be
  // in the nursery then we need to check for that here and skip the barrier (we
  // only need to record pointers from the tenured heap into the nursery).
  MOZ_ASSERT(object.isNothing());

  // Push `object` and `value` to preserve them across the call.
  if (object) {
    pushRef(*object);
  }
  pushRef(value);

  // Push the arguments and call the precise post-barrier
  pushPtr(valueAddr);
  pushRef(prevValue);
  if (!emitInstanceCall(SASigPostBarrierEdgePrecise)) {
    return false;
  }

  // Restore `object` and `value`.
  popRef(value);
  if (object) {
    popRef(*object);
  }

  return true;
}

bool BaseCompiler::emitBarrieredStore(const Maybe<RegRef>& object,
                                      RegPtr valueAddr, RegRef value,
                                      PreBarrierKind preBarrierKind,
                                      PostBarrierKind postBarrierKind) {
  // The pre-barrier preserves all allocated registers.
  if (preBarrierKind == PreBarrierKind::Normal) {
    emitPreBarrier(valueAddr);
  }

  // The precise post-barrier requires the previous value stored in the field,
  // in order to know if the previous store buffer entry needs to be removed.
  RegRef prevValue;
  if (postBarrierKind == PostBarrierKind::Precise) {
    prevValue = needRef();
    masm.loadPtr(Address(valueAddr, 0), prevValue);
  }

  // Store the value
  masm.storePtr(value, Address(valueAddr, 0));

  // The post-barrier preserves object and value.
  switch (postBarrierKind) {
    case PostBarrierKind::None:
      freePtr(valueAddr);
      return true;
    case PostBarrierKind::Imprecise:
      return emitPostBarrierEdgeImprecise(object, valueAddr, value);
    case PostBarrierKind::Precise:
      return emitPostBarrierEdgePrecise(object, valueAddr, prevValue, value);
    case PostBarrierKind::WholeCell:
      // valueAddr is reused as a temp register.
      return emitPostBarrierWholeCell(object.value(), value, valueAddr);
    default:
      MOZ_CRASH("unknown barrier kind");
  }
}

void BaseCompiler::emitBarrieredClear(RegPtr valueAddr) {
  // The pre-barrier preserves all allocated registers.
  emitPreBarrier(valueAddr);

  // Store null
  masm.storePtr(ImmWord(AnyRef::NullRefValue), Address(valueAddr, 0));

  // No post-barrier is needed, as null does not require a store buffer entry
}

//////////////////////////////////////////////////////////////////////////////
//
// GC proposal.

RegPtr BaseCompiler::loadAllocSiteInstanceData(uint32_t allocSiteIndex) {
  RegPtr rp = needPtr();
  RegPtr instance;
#ifndef RABALDR_PIN_INSTANCE
  instance = rp;
  fr.loadInstancePtr(instance);
#else
  // We can use the pinned instance register.
  instance = RegPtr(InstanceReg);
#endif
  masm.loadPtr(Address(instance, Instance::offsetOfAllocSites()), rp);

  ScratchI32 scratch(*this);
  CodeOffset offset = masm.move32WithPatch(scratch);
  masm.allocSitesPatches()[allocSiteIndex].setPatchOffset(offset.offset());
  masm.addPtr(scratch, rp);
  return rp;
}

bool BaseCompiler::readAllocSiteIndex(uint32_t* index) {
  *index = masm.allocSitesPatches().length();
  masm.append(wasm::AllocSitePatch());
  return !masm.oom();
}

RegPtr BaseCompiler::loadSuperTypeVector(uint32_t typeIndex) {
  RegPtr rp = needPtr();
  RegPtr instance;
#ifndef RABALDR_PIN_INSTANCE
  // We need to load the instance register, but can use the destination
  // register as a temporary.
  instance = rp;
  fr.loadInstancePtr(rp);
#else
  // We can use the pinned instance register.
  instance = RegPtr(InstanceReg);
#endif
  masm.loadPtr(
      Address(instance, Instance::offsetInData(
                            codeMeta_.offsetOfSuperTypeVector(typeIndex))),
      rp);
  return rp;
}

/* static */
void BaseCompiler::SignalNullCheck::emitNullCheck(BaseCompiler* bc, RegRef rp) {
  Label ok;
  MacroAssembler& masm = bc->masm;
  masm.branchTestPtr(Assembler::NonZero, rp, rp, &ok);
  bc->trap(Trap::NullPointerDereference);
  masm.bind(&ok);
}

/* static */
void BaseCompiler::SignalNullCheck::emitTrapSite(BaseCompiler* bc,
                                                 FaultingCodeRange fcr,
                                                 TrapMachineInsn tmi) {
  MacroAssembler& masm = bc->masm;
  masm.appendAndVerify(wasm::Trap::NullPointerDereference, tmi, fcr,
                       bc->trapSiteDesc());
}

template <typename NullCheckPolicy>
RegPtr BaseCompiler::emitGcArrayGetData(RegRef rp) {
  // `rp` points at a WasmArrayObject.  Return a reg holding the value of its
  // `data_` field.
  RegPtr rdata = needPtr();
  FaultingCodeRange fcr =
      masm.loadPtr(Address(rp, WasmArrayObject::offsetOfData()), rdata);
  NullCheckPolicy::emitTrapSite(this, fcr, TrapMachineInsnForLoadWord());
  return rdata;
}

template <typename NullCheckPolicy>
RegI32 BaseCompiler::emitGcArrayGetNumElements(RegRef rp) {
  // `rp` points at a WasmArrayObject.  Return a reg holding the value of its
  // `numElements_` field.
  STATIC_ASSERT_WASMARRAYELEMENTS_NUMELEMENTS_IS_U32;
  RegI32 numElements = needI32();
  FaultingCodeRange fcr = masm.load32(
      Address(rp, WasmArrayObject::offsetOfNumElements()), numElements);
  NullCheckPolicy::emitTrapSite(this, fcr, TrapMachineInsn::Load32);
  return numElements;
}

void BaseCompiler::emitGcArrayBoundsCheck(RegI32 index, RegI32 numElements) {
  Label inBounds;
  masm.branch32(Assembler::Below, index, numElements, &inBounds);
  trap(Trap::OutOfBounds);
  masm.bind(&inBounds);
}

template <typename T, typename NullCheckPolicy>
void BaseCompiler::emitGcGet(StorageType type, FieldWideningOp wideningOp,
                             const T& src) {
  switch (type.kind()) {
    case StorageType::I8: {
      MOZ_ASSERT(wideningOp != FieldWideningOp::None);
      RegI32 r = needI32();
      FaultingCodeRange fcr;
      if (wideningOp == FieldWideningOp::Unsigned) {
        fcr = masm.load8ZeroExtend(src, r);
      } else {
        fcr = masm.load8SignExtend(src, r);
      }
      NullCheckPolicy::emitTrapSite(this, fcr, TrapMachineInsn::Load8);
      pushI32(r);
      break;
    }
    case StorageType::I16: {
      MOZ_ASSERT(wideningOp != FieldWideningOp::None);
      RegI32 r = needI32();
      FaultingCodeRange fcr;
      if (wideningOp == FieldWideningOp::Unsigned) {
        fcr = masm.load16ZeroExtend(src, r);
      } else {
        fcr = masm.load16SignExtend(src, r);
      }
      NullCheckPolicy::emitTrapSite(this, fcr, TrapMachineInsn::Load16);
      pushI32(r);
      break;
    }
    case StorageType::I32: {
      MOZ_ASSERT(wideningOp == FieldWideningOp::None);
      RegI32 r = needI32();
      FaultingCodeRange fcr = masm.load32(src, r);
      NullCheckPolicy::emitTrapSite(this, fcr, TrapMachineInsn::Load32);
      pushI32(r);
      break;
    }
    case StorageType::I64: {
      MOZ_ASSERT(wideningOp == FieldWideningOp::None);
      RegI64 r = needI64();
#ifdef JS_64BIT
      FaultingCodeRange fcr = masm.load64(src, r);
      NullCheckPolicy::emitTrapSite(this, fcr, TrapMachineInsn::Load64);
#else
      FaultingCodeRangePair fcrp = masm.load64(src, r);
      NullCheckPolicy::emitTrapSite(this, fcrp.first, TrapMachineInsn::Load32);
      NullCheckPolicy::emitTrapSite(this, fcrp.second, TrapMachineInsn::Load32);
#endif
      pushI64(r);
      break;
    }
    case StorageType::F32: {
      MOZ_ASSERT(wideningOp == FieldWideningOp::None);
      RegF32 r = needF32();
      FaultingCodeRange fcr = masm.loadFloat32(src, r);
      NullCheckPolicy::emitTrapSite(this, fcr, TrapMachineInsn::Load32);
      pushF32(r);
      break;
    }
    case StorageType::F64: {
      MOZ_ASSERT(wideningOp == FieldWideningOp::None);
      RegF64 r = needF64();
      FaultingCodeRange fcr = masm.loadDouble(src, r);
      NullCheckPolicy::emitTrapSite(this, fcr, TrapMachineInsn::Load64);
      pushF64(r);
      break;
    }
#ifdef ENABLE_WASM_SIMD
    case StorageType::V128: {
      MOZ_ASSERT(wideningOp == FieldWideningOp::None);
      RegV128 r = needV128();
      FaultingCodeRange fcr = masm.loadUnalignedSimd128(src, r);
      NullCheckPolicy::emitTrapSite(this, fcr, TrapMachineInsn::Load128);
      pushV128(r);
      break;
    }
#endif
    case StorageType::Ref: {
      MOZ_ASSERT(wideningOp == FieldWideningOp::None);
      RegRef r = needRef();
      FaultingCodeRange fcr = masm.loadPtr(src, r);
      NullCheckPolicy::emitTrapSite(this, fcr, TrapMachineInsnForLoadWord());
      pushRef(r);
      break;
    }
    default: {
      MOZ_CRASH("Unexpected field type");
    }
  }
}

template <typename T, typename NullCheckPolicy>
void BaseCompiler::emitGcSetScalar(const T& dst, StorageType type,
                                   AnyReg value) {
  switch (type.kind()) {
    case StorageType::I8: {
      FaultingCodeRange fcr = masm.store8(value.i32(), dst);
      NullCheckPolicy::emitTrapSite(this, fcr, TrapMachineInsn::Store8);
      break;
    }
    case StorageType::I16: {
      FaultingCodeRange fcr = masm.store16(value.i32(), dst);
      NullCheckPolicy::emitTrapSite(this, fcr, TrapMachineInsn::Store16);
      break;
    }
    case StorageType::I32: {
      FaultingCodeRange fcr = masm.store32(value.i32(), dst);
      NullCheckPolicy::emitTrapSite(this, fcr, TrapMachineInsn::Store32);
      break;
    }
    case StorageType::I64: {
#ifdef JS_64BIT
      FaultingCodeRange fcr = masm.store64(value.i64(), dst);
      NullCheckPolicy::emitTrapSite(this, fcr, TrapMachineInsn::Store64);
#else
      FaultingCodeRangePair fcrp = masm.store64(value.i64(), dst);
      NullCheckPolicy::emitTrapSite(this, fcrp.first, TrapMachineInsn::Store32);
      NullCheckPolicy::emitTrapSite(this, fcrp.second,
                                    TrapMachineInsn::Store32);
#endif
      break;
    }
    case StorageType::F32: {
      FaultingCodeRange fcr = masm.storeFloat32(value.f32(), dst);
      NullCheckPolicy::emitTrapSite(this, fcr, TrapMachineInsn::Store32);
      break;
    }
    case StorageType::F64: {
      FaultingCodeRange fcr = masm.storeDouble(value.f64(), dst);
      NullCheckPolicy::emitTrapSite(this, fcr, TrapMachineInsn::Store64);
      break;
    }
#ifdef ENABLE_WASM_SIMD
    case StorageType::V128: {
      FaultingCodeRange fcr = masm.storeUnalignedSimd128(value.v128(), dst);
      NullCheckPolicy::emitTrapSite(this, fcr, TrapMachineInsn::Store128);
      break;
    }
#endif
    default: {
      MOZ_CRASH("Unexpected field type");
    }
  }
}

template <typename NullCheckPolicy>
bool BaseCompiler::emitGcStructSet(RegRef object, RegPtr areaBase,
                                   uint32_t areaOffset, StorageType type,
                                   AnyReg value,
                                   PreBarrierKind preBarrierKind) {
  // Easy path if the field is a scalar
  if (!type.isRefRepr()) {
    emitGcSetScalar<Address, NullCheckPolicy>(Address(areaBase, areaOffset),
                                              type, value);
    freeAny(value);
    return true;
  }

  // Create temporary for the valueAddr that is not in the prebarrier register
  // and can be consumed by the barrier operation
  RegPtr valueAddr = RegPtr(PreBarrierReg);
  needPtr(valueAddr);
  masm.computeEffectiveAddress(Address(areaBase, areaOffset), valueAddr);

  NullCheckPolicy::emitNullCheck(this, object);

  // emitBarrieredStore preserves object and value
  if (!emitBarrieredStore(Some(object), valueAddr, value.ref(), preBarrierKind,
                          PostBarrierKind::WholeCell)) {
    return false;
  }
  freeRef(value.ref());

  return true;
}

bool BaseCompiler::emitGcArraySet(RegRef object, RegPtr data, RegI32 index,
                                  const ArrayType& arrayType, AnyReg value,
                                  PreBarrierKind preBarrierKind,
                                  PostBarrierKind postBarrierKind) {
  // Try to use a base index store instruction if the field type fits in a
  // shift immediate. If not we shift the index manually and then unshift
  // it after the store. We don't use an extra register for this because we
  // don't have any to spare on x86.
  uint32_t shift = arrayType.elementType().indexingShift();
  Scale scale;
  bool shiftedIndex = false;
  if (IsShiftInScaleRange(shift)) {
    scale = ShiftToScale(shift);
  } else {
    masm.lshiftPtr(Imm32(shift), index);
    scale = TimesOne;
    shiftedIndex = true;
  }
  auto unshiftIndex = mozilla::MakeScopeExit([&] {
    if (shiftedIndex) {
      masm.rshiftPtr(Imm32(shift), index);
    }
  });

  // Easy path if the field is a scalar
  if (!arrayType.elementType().isRefRepr()) {
    emitGcSetScalar<BaseIndex, NoNullCheck>(BaseIndex(data, index, scale, 0),
                                            arrayType.elementType(), value);
    return true;
  }

  // Create temporaries for valueAddr that is not in the prebarrier register
  // and can be consumed by the barrier operation
  RegPtr valueAddr = RegPtr(PreBarrierReg);
  needPtr(valueAddr);
  masm.computeEffectiveAddress(BaseIndex(data, index, scale, 0), valueAddr);

  // Save state for after barriered write
  pushPtr(data);
  pushI32(index);

  // emitBarrieredStore preserves object and value
  if (!emitBarrieredStore(Some(object), valueAddr, value.ref(), preBarrierKind,
                          postBarrierKind)) {
    return false;
  }

  // Restore state
  popI32(index);
  popPtr(data);

  return true;
}

template <bool ZeroFields>
bool BaseCompiler::emitStructAlloc(uint32_t typeIndex, RegRef* object,
                                   bool* isOutlineStruct, RegPtr* outlineBase,
                                   uint32_t allocSiteIndex) {
  const TypeDef& typeDef = (*codeMeta_.types)[typeIndex];
  const StructType& structType = typeDef.structType();
  gc::AllocKind allocKind = structType.allocKind_;

  *isOutlineStruct = structType.hasOOL();

  // Reserve this register early if we will need it so that it is not taken by
  // any register used in this function.
  needPtr(RegPtr(PreBarrierReg));

  *object = RegRef();

  // Allocate an uninitialized struct. This requires the type definition
  // for the struct to be pushed on the stack. This will trap on OOM.
  if (*isOutlineStruct) {
    pushI32(int32_t(typeIndex));
    pushPtr(loadAllocSiteInstanceData(allocSiteIndex));
    if (!emitInstanceCall(ZeroFields ? SASigStructNewOOL_true
                                     : SASigStructNewOOL_false)) {
      return false;
    }
    *object = popRef();
  } else {
    // We eagerly sync the value stack to the machine stack here so as not to
    // confuse things with the conditional instance call below.
    sync();

    RegPtr instance;
    *object = RegRef(ReturnReg);
    needRef(*object);
#ifndef RABALDR_PIN_INSTANCE
    // We reuse the result register for the instance.
    instance = needPtr();
    fr.loadInstancePtr(instance);
#else
    // We can use the pinned instance register.
    instance = RegPtr(InstanceReg);
#endif

    RegPtr allocSite = loadAllocSiteInstanceData(allocSiteIndex);

    size_t offsetOfTypeDefData = wasm::Instance::offsetInData(
        codeMeta_.offsetOfTypeDefInstanceData(typeIndex));

    Label success;
    Label fail;
    {
      ScratchPtr scratch(*this);

      masm.wasmNewStructObject(instance, *object, allocSite, scratch,
                               offsetOfTypeDefData, &fail, allocKind,
                               ZeroFields);
    }
    masm.jump(&success);

    masm.bind(&fail);
#ifndef RABALDR_PIN_INSTANCE
    freePtr(instance);
#endif
    freeRef(*object);
    pushI32(int32_t(typeIndex));
    pushPtr(allocSite);
    if (!emitInstanceCall(ZeroFields ? SASigStructNewIL_true
                                     : SASigStructNewIL_false)) {
      return false;
    }
    *object = popRef();
    MOZ_ASSERT(*object == RegRef(ReturnReg));

    masm.bind(&success);
  }

  *outlineBase = *isOutlineStruct ? needPtr() : RegPtr();

  // Free the barrier reg for later use
  freePtr(RegPtr(PreBarrierReg));

  return true;
}





template <bool ZeroFields>
bool BaseCompiler::emitArrayAlloc(uint32_t typeIndex, RegRef object,
                                  RegI32 numElements, uint32_t elemSize,
                                  uint32_t allocSiteIndex) {
  // We eagerly sync the value stack to the machine stack here so as not to
  // confuse things with the conditional instance call below.
  sync();

  RegPtr instance;
#ifndef RABALDR_PIN_INSTANCE
  // We reuse the object register for the instance. This is ok because object is
  // not live until instance is dead.
  instance = needPtr();
  fr.loadInstancePtr(instance);
#else
  // We can use the pinned instance register.
  instance = RegPtr(InstanceReg);
#endif

  RegPtr allocSite = loadAllocSiteInstanceData(allocSiteIndex);

  size_t offsetOfTypeDefData = wasm::Instance::offsetInData(
      codeMeta_.offsetOfTypeDefInstanceData(typeIndex));

  Label success;
  Label fail;
  {
    ScratchPtr scratch(*this);
    masm.wasmNewArrayObject(instance, object, numElements, allocSite, scratch,
                            offsetOfTypeDefData, &fail, elemSize, ZeroFields);
  }
  masm.jump(&success);

  masm.bind(&fail);
#ifndef RABALDR_PIN_INSTANCE
  freePtr(instance);
#endif
  freeRef(object);
  pushI32(numElements);
  pushI32(int32_t(typeIndex));
  pushPtr(allocSite);
  if (!emitInstanceCall(ZeroFields ? SASigArrayNew_true
                                   : SASigArrayNew_false)) {
    return false;
  }
  popRef(object);

  masm.bind(&success);
  return true;
}

template <bool ZeroFields>
bool BaseCompiler::emitArrayAllocFixed(uint32_t typeIndex, RegRef object,
                                       uint32_t numElements, uint32_t elemSize,
                                       uint32_t allocSiteIndex) {
  SymbolicAddressSignature fun =
      ZeroFields ? SASigArrayNew_true : SASigArrayNew_false;

  // The maximum number of elements for array.new_fixed enforced in validation
  // should always prevent overflow here.
  static_assert(MaxArrayNewFixedElements * sizeof(wasm::LitVal) <
                MaxArrayPayloadBytes);
  uint32_t storageBytes =
      WasmArrayObject::calcArrayDataBytesUnchecked(elemSize, numElements);
  if (storageBytes > WasmArrayObject_MaxInlineBytes) {
    freeRef(object);
    pushI32(numElements);
    pushI32(int32_t(typeIndex));
    pushPtr(loadAllocSiteInstanceData(allocSiteIndex));
    if (!emitInstanceCall(fun)) {
      return false;
    }
    popRef(object);

    return true;
  }

  // We eagerly sync the value stack to the machine stack here so as not to
  // confuse things with the conditional instance call below.
  sync();

  RegPtr instance;
#ifndef RABALDR_PIN_INSTANCE
  // We reuse the object register for the instance. This is ok because object is
  // not live until instance is dead.
  instance = needPtr();
  fr.loadInstancePtr(instance);
#else
  // We can use the pinned instance register.
  instance = RegPtr(InstanceReg);
#endif

  RegPtr allocSite = loadAllocSiteInstanceData(allocSiteIndex);
  RegPtr temp1 = needPtr();

  size_t offsetOfTypeDefData = wasm::Instance::offsetInData(
      codeMeta_.offsetOfTypeDefInstanceData(typeIndex));

  Label success;
  Label fail;
  {
    ScratchPtr scratch(*this);
    masm.wasmNewArrayObjectFixed(instance, object, allocSite, temp1, scratch,
                                 offsetOfTypeDefData, &fail, numElements,
                                 storageBytes, ZeroFields);
  }
  freePtr(temp1);
  masm.jump(&success);

  masm.bind(&fail);
#ifndef RABALDR_PIN_INSTANCE
  freePtr(instance);
#endif
  freeRef(object);
  pushI32(numElements);
  pushI32(int32_t(typeIndex));
  pushPtr(allocSite);
  if (!emitInstanceCall(fun)) {
    return false;
  }
  popRef(object);

  masm.bind(&success);

  return true;
}















BranchIfRefSubtypeRegisters BaseCompiler::allocRegistersForBranchIfRefSubtype(
    RefType destType) {
  BranchWasmRefIsSubtypeRegisters needs =
      MacroAssembler::regsForBranchWasmRefIsSubtype(destType);
  return BranchIfRefSubtypeRegisters{
      .superSTV = needs.needSuperSTV
                      ? loadSuperTypeVector(
                            codeMeta_.types->indexOf(*destType.typeDef()))
                      : RegPtr::Invalid(),
      .scratch1 = needs.needScratch1 ? needI32() : RegI32::Invalid(),
      .scratch2 = needs.needScratch2 ? needI32() : RegI32::Invalid(),
  };
}

void BaseCompiler::freeRegistersForBranchIfRefSubtype(
    const BranchIfRefSubtypeRegisters& regs) {
  if (regs.superSTV.isValid()) {
    freePtr(regs.superSTV);
  }
  if (regs.scratch1.isValid()) {
    freeI32(regs.scratch1);
  }
  if (regs.scratch2.isValid()) {
    freeI32(regs.scratch2);
  }
}



bool BaseCompiler::emitBrOnCastCommon(bool onSuccess,
                                      uint32_t labelRelativeDepth,
                                      const ResultType& labelType,
                                      MaybeRefType sourceType,
                                      RefType destType) {
  Control& target = controlItem(labelRelativeDepth);
  target.bceSafeOnExit &= bceSafe_;

  // 3. br_if $l : [T*, ref] -> [T*, ref]
  BranchState b(&target.label, target.stackHeight, InvertBranch(false),
                labelType);

  // Don't allocate the result register used in the branch
  if (b.hasBlockResults()) {
    needIntegerResultRegisters(b.resultType);
  }

  // Get the ref from the top of the stack
  RegRef refCondition = popRef();

  // Create a copy of the ref for passing to the on_cast label,
  // the original ref is used in the condition.
  RegRef ref = needRef();
  moveRef(refCondition, ref);
  pushRef(ref);

  if (b.hasBlockResults()) {
    freeIntegerResultRegisters(b.resultType);
  }

  if (!jumpConditionalWithResults(&b, refCondition, sourceType, destType,
                                  onSuccess)) {
    return false;
  }
  freeRef(refCondition);

  return true;
}




//////////////////////////////////////////////////////////////////////////////
//
// SIMD and Relaxed SIMD.

#ifdef ENABLE_WASM_SIMD

// Emitter trampolines used by abstracted SIMD operations.  Naming here follows
// the SIMD spec pretty closely.

static void AndV128(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.bitwiseAndSimd128(rs, rsd);
}

static void OrV128(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.bitwiseOrSimd128(rs, rsd);
}

static void XorV128(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.bitwiseXorSimd128(rs, rsd);
}

static void AddI8x16(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.addInt8x16(rsd, rs, rsd);
}

static void AddI16x8(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.addInt16x8(rsd, rs, rsd);
}

static void AddI32x4(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.addInt32x4(rsd, rs, rsd);
}

static void AddF32x4(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.addFloat32x4(rsd, rs, rsd);
}

static void AddI64x2(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.addInt64x2(rsd, rs, rsd);
}

static void AddF64x2(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.addFloat64x2(rsd, rs, rsd);
}

static void AddSatI8x16(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.addSatInt8x16(rsd, rs, rsd);
}

static void AddSatUI8x16(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.unsignedAddSatInt8x16(rsd, rs, rsd);
}

static void AddSatI16x8(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.addSatInt16x8(rsd, rs, rsd);
}

static void AddSatUI16x8(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.unsignedAddSatInt16x8(rsd, rs, rsd);
}

static void SubI8x16(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.subInt8x16(rsd, rs, rsd);
}

static void SubI16x8(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.subInt16x8(rsd, rs, rsd);
}

static void SubI32x4(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.subInt32x4(rsd, rs, rsd);
}

static void SubF32x4(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.subFloat32x4(rsd, rs, rsd);
}

static void SubI64x2(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.subInt64x2(rsd, rs, rsd);
}

static void SubF64x2(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.subFloat64x2(rsd, rs, rsd);
}

static void SubSatI8x16(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.subSatInt8x16(rsd, rs, rsd);
}

static void SubSatUI8x16(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.unsignedSubSatInt8x16(rsd, rs, rsd);
}

static void SubSatI16x8(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.subSatInt16x8(rsd, rs, rsd);
}

static void SubSatUI16x8(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.unsignedSubSatInt16x8(rsd, rs, rsd);
}

static void MulI16x8(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.mulInt16x8(rsd, rs, rsd);
}

static void MulI32x4(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.mulInt32x4(rsd, rs, rsd);
}

static void MulF32x4(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.mulFloat32x4(rsd, rs, rsd);
}

#  if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
static void MulI64x2(MacroAssembler& masm, RegV128 rs, RegV128 rsd,
                     RegV128 temp) {
  masm.mulInt64x2(rsd, rs, rsd, temp);
}
#  elif defined(JS_CODEGEN_ARM64)
static void MulI64x2(MacroAssembler& masm, RegV128 rs, RegV128 rsd,
                     RegV128 temp1, RegV128 temp2) {
  masm.mulInt64x2(rsd, rs, rsd, temp1, temp2);
}
#  endif

static void MulF64x2(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.mulFloat64x2(rsd, rs, rsd);
}

static void DivF32x4(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.divFloat32x4(rsd, rs, rsd);
}

static void DivF64x2(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.divFloat64x2(rsd, rs, rsd);
}

#  if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
static void MinF32x4(MacroAssembler& masm, RegV128 rs, RegV128 rsd,
                     RegV128 temp1, RegV128 temp2) {
  masm.minFloat32x4(rsd, rs, rsd, temp1, temp2);
}

static void MinF64x2(MacroAssembler& masm, RegV128 rs, RegV128 rsd,
                     RegV128 temp1, RegV128 temp2) {
  masm.minFloat64x2(rsd, rs, rsd, temp1, temp2);
}

static void MaxF32x4(MacroAssembler& masm, RegV128 rs, RegV128 rsd,
                     RegV128 temp1, RegV128 temp2) {
  masm.maxFloat32x4(rsd, rs, rsd, temp1, temp2);
}

static void MaxF64x2(MacroAssembler& masm, RegV128 rs, RegV128 rsd,
                     RegV128 temp1, RegV128 temp2) {
  masm.maxFloat64x2(rsd, rs, rsd, temp1, temp2);
}

static void PMinF32x4(MacroAssembler& masm, RegV128 rsd, RegV128 rs,
                      RhsDestOp) {
  masm.pseudoMinFloat32x4(rsd, rs);
}

static void PMinF64x2(MacroAssembler& masm, RegV128 rsd, RegV128 rs,
                      RhsDestOp) {
  masm.pseudoMinFloat64x2(rsd, rs);
}

static void PMaxF32x4(MacroAssembler& masm, RegV128 rsd, RegV128 rs,
                      RhsDestOp) {
  masm.pseudoMaxFloat32x4(rsd, rs);
}

static void PMaxF64x2(MacroAssembler& masm, RegV128 rsd, RegV128 rs,
                      RhsDestOp) {
  masm.pseudoMaxFloat64x2(rsd, rs);
}
#  elif defined(JS_CODEGEN_ARM64)
static void MinF32x4(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.minFloat32x4(rs, rsd);
}

static void MinF64x2(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.minFloat64x2(rs, rsd);
}

static void MaxF32x4(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.maxFloat32x4(rs, rsd);
}

static void MaxF64x2(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.maxFloat64x2(rs, rsd);
}

static void PMinF32x4(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.pseudoMinFloat32x4(rs, rsd);
}

static void PMinF64x2(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.pseudoMinFloat64x2(rs, rsd);
}

static void PMaxF32x4(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.pseudoMaxFloat32x4(rs, rsd);
}

static void PMaxF64x2(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.pseudoMaxFloat64x2(rs, rsd);
}
#  endif

static void DotI16x8(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.widenDotInt16x8(rsd, rs, rsd);
}

static void ExtMulLowI8x16(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.extMulLowInt8x16(rsd, rs, rsd);
}

static void ExtMulHighI8x16(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.extMulHighInt8x16(rsd, rs, rsd);
}

static void ExtMulLowUI8x16(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.unsignedExtMulLowInt8x16(rsd, rs, rsd);
}

static void ExtMulHighUI8x16(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.unsignedExtMulHighInt8x16(rsd, rs, rsd);
}

static void ExtMulLowI16x8(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.extMulLowInt16x8(rsd, rs, rsd);
}

static void ExtMulHighI16x8(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.extMulHighInt16x8(rsd, rs, rsd);
}

static void ExtMulLowUI16x8(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.unsignedExtMulLowInt16x8(rsd, rs, rsd);
}

static void ExtMulHighUI16x8(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.unsignedExtMulHighInt16x8(rsd, rs, rsd);
}

static void ExtMulLowI32x4(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.extMulLowInt32x4(rsd, rs, rsd);
}

static void ExtMulHighI32x4(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.extMulHighInt32x4(rsd, rs, rsd);
}

static void ExtMulLowUI32x4(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.unsignedExtMulLowInt32x4(rsd, rs, rsd);
}

static void ExtMulHighUI32x4(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.unsignedExtMulHighInt32x4(rsd, rs, rsd);
}

static void Q15MulrSatS(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.q15MulrSatInt16x8(rsd, rs, rsd);
}

static void CmpI8x16(MacroAssembler& masm, Assembler::Condition cond,
                     RegV128 rs, RegV128 rsd) {
  masm.compareInt8x16(cond, rs, rsd);
}

static void CmpI16x8(MacroAssembler& masm, Assembler::Condition cond,
                     RegV128 rs, RegV128 rsd) {
  masm.compareInt16x8(cond, rs, rsd);
}

static void CmpI32x4(MacroAssembler& masm, Assembler::Condition cond,
                     RegV128 rs, RegV128 rsd) {
  masm.compareInt32x4(cond, rs, rsd);
}

#  if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
static void CmpI64x2ForEquality(MacroAssembler& masm, Assembler::Condition cond,
                                RegV128 rs, RegV128 rsd) {
  masm.compareForEqualityInt64x2(cond, rsd, rs, rsd);
}

static void CmpI64x2ForOrdering(MacroAssembler& masm, Assembler::Condition cond,
                                RegV128 rs, RegV128 rsd, RegV128 temp1,
                                RegV128 temp2) {
  masm.compareForOrderingInt64x2(cond, rsd, rs, rsd, temp1, temp2);
}
#  else
static void CmpI64x2ForEquality(MacroAssembler& masm, Assembler::Condition cond,
                                RegV128 rs, RegV128 rsd) {
  masm.compareInt64x2(cond, rs, rsd);
}

static void CmpI64x2ForOrdering(MacroAssembler& masm, Assembler::Condition cond,
                                RegV128 rs, RegV128 rsd) {
  masm.compareInt64x2(cond, rs, rsd);
}
#  endif  // JS_CODEGEN_X86 || JS_CODEGEN_X64

static void CmpUI8x16(MacroAssembler& masm, Assembler::Condition cond,
                      RegV128 rs, RegV128 rsd) {
  masm.compareInt8x16(cond, rs, rsd);
}

static void CmpUI16x8(MacroAssembler& masm, Assembler::Condition cond,
                      RegV128 rs, RegV128 rsd) {
  masm.compareInt16x8(cond, rs, rsd);
}

static void CmpUI32x4(MacroAssembler& masm, Assembler::Condition cond,
                      RegV128 rs, RegV128 rsd) {
  masm.compareInt32x4(cond, rs, rsd);
}

static void CmpF32x4(MacroAssembler& masm, Assembler::Condition cond,
                     RegV128 rs, RegV128 rsd) {
  masm.compareFloat32x4(cond, rs, rsd);
}

static void CmpF64x2(MacroAssembler& masm, Assembler::Condition cond,
                     RegV128 rs, RegV128 rsd) {
  masm.compareFloat64x2(cond, rs, rsd);
}

static void NegI8x16(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.negInt8x16(rs, rd);
}

static void NegI16x8(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.negInt16x8(rs, rd);
}

static void NegI32x4(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.negInt32x4(rs, rd);
}

static void NegI64x2(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.negInt64x2(rs, rd);
}

static void NegF32x4(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.negFloat32x4(rs, rd);
}

static void NegF64x2(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.negFloat64x2(rs, rd);
}

static void AbsF32x4(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.absFloat32x4(rs, rd);
}

static void AbsF64x2(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.absFloat64x2(rs, rd);
}

static void SqrtF32x4(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.sqrtFloat32x4(rs, rd);
}

static void SqrtF64x2(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.sqrtFloat64x2(rs, rd);
}

static void CeilF32x4(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.ceilFloat32x4(rs, rd);
}

static void FloorF32x4(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.floorFloat32x4(rs, rd);
}

static void TruncF32x4(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.truncFloat32x4(rs, rd);
}

static void NearestF32x4(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.nearestFloat32x4(rs, rd);
}

static void CeilF64x2(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.ceilFloat64x2(rs, rd);
}

static void FloorF64x2(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.floorFloat64x2(rs, rd);
}

static void TruncF64x2(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.truncFloat64x2(rs, rd);
}

static void NearestF64x2(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.nearestFloat64x2(rs, rd);
}

static void NotV128(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.bitwiseNotSimd128(rs, rd);
}

static void ExtAddPairwiseI8x16(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.extAddPairwiseInt8x16(rs, rsd);
}

static void ExtAddPairwiseUI8x16(MacroAssembler& masm, RegV128 rs,
                                 RegV128 rsd) {
  masm.unsignedExtAddPairwiseInt8x16(rs, rsd);
}

static void ExtAddPairwiseI16x8(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.extAddPairwiseInt16x8(rs, rsd);
}

static void ExtAddPairwiseUI16x8(MacroAssembler& masm, RegV128 rs,
                                 RegV128 rsd) {
  masm.unsignedExtAddPairwiseInt16x8(rs, rsd);
}

static void ShiftOpMask(MacroAssembler& masm, SimdOp op, RegI32 in,
                        RegI32 out) {
  int32_t maskBits;

  masm.mov(in, out);
  if (MacroAssembler::MustMaskShiftCountSimd128(op, &maskBits)) {
    masm.and32(Imm32(maskBits), out);
  }
}

#  if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
static void ShiftLeftI8x16(MacroAssembler& masm, RegI32 rs, RegV128 rsd,
                           RegI32 temp1, RegV128 temp2) {
  ShiftOpMask(masm, SimdOp::I8x16Shl, rs, temp1);
  masm.leftShiftInt8x16(temp1, rsd, temp2);
}

static void ShiftLeftI16x8(MacroAssembler& masm, RegI32 rs, RegV128 rsd,
                           RegI32 temp) {
  ShiftOpMask(masm, SimdOp::I16x8Shl, rs, temp);
  masm.leftShiftInt16x8(temp, rsd);
}

static void ShiftLeftI32x4(MacroAssembler& masm, RegI32 rs, RegV128 rsd,
                           RegI32 temp) {
  ShiftOpMask(masm, SimdOp::I32x4Shl, rs, temp);
  masm.leftShiftInt32x4(temp, rsd);
}

static void ShiftLeftI64x2(MacroAssembler& masm, RegI32 rs, RegV128 rsd,
                           RegI32 temp) {
  ShiftOpMask(masm, SimdOp::I64x2Shl, rs, temp);
  masm.leftShiftInt64x2(temp, rsd);
}

static void ShiftRightI8x16(MacroAssembler& masm, RegI32 rs, RegV128 rsd,
                            RegI32 temp1, RegV128 temp2) {
  ShiftOpMask(masm, SimdOp::I8x16ShrS, rs, temp1);
  masm.rightShiftInt8x16(temp1, rsd, temp2);
}

static void ShiftRightUI8x16(MacroAssembler& masm, RegI32 rs, RegV128 rsd,
                             RegI32 temp1, RegV128 temp2) {
  ShiftOpMask(masm, SimdOp::I8x16ShrU, rs, temp1);
  masm.unsignedRightShiftInt8x16(temp1, rsd, temp2);
}

static void ShiftRightI16x8(MacroAssembler& masm, RegI32 rs, RegV128 rsd,
                            RegI32 temp) {
  ShiftOpMask(masm, SimdOp::I16x8ShrS, rs, temp);
  masm.rightShiftInt16x8(temp, rsd);
}

static void ShiftRightUI16x8(MacroAssembler& masm, RegI32 rs, RegV128 rsd,
                             RegI32 temp) {
  ShiftOpMask(masm, SimdOp::I16x8ShrU, rs, temp);
  masm.unsignedRightShiftInt16x8(temp, rsd);
}

static void ShiftRightI32x4(MacroAssembler& masm, RegI32 rs, RegV128 rsd,
                            RegI32 temp) {
  ShiftOpMask(masm, SimdOp::I32x4ShrS, rs, temp);
  masm.rightShiftInt32x4(temp, rsd);
}

static void ShiftRightUI32x4(MacroAssembler& masm, RegI32 rs, RegV128 rsd,
                             RegI32 temp) {
  ShiftOpMask(masm, SimdOp::I32x4ShrU, rs, temp);
  masm.unsignedRightShiftInt32x4(temp, rsd);
}

static void ShiftRightUI64x2(MacroAssembler& masm, RegI32 rs, RegV128 rsd,
                             RegI32 temp) {
  ShiftOpMask(masm, SimdOp::I64x2ShrU, rs, temp);
  masm.unsignedRightShiftInt64x2(temp, rsd);
}
#  elif defined(JS_CODEGEN_ARM64)
static void ShiftLeftI8x16(MacroAssembler& masm, RegI32 rs, RegV128 rsd,
                           RegI32 temp) {
  ShiftOpMask(masm, SimdOp::I8x16Shl, rs, temp);
  masm.leftShiftInt8x16(rsd, temp, rsd);
}

static void ShiftLeftI16x8(MacroAssembler& masm, RegI32 rs, RegV128 rsd,
                           RegI32 temp) {
  ShiftOpMask(masm, SimdOp::I16x8Shl, rs, temp);
  masm.leftShiftInt16x8(rsd, temp, rsd);
}

static void ShiftLeftI32x4(MacroAssembler& masm, RegI32 rs, RegV128 rsd,
                           RegI32 temp) {
  ShiftOpMask(masm, SimdOp::I32x4Shl, rs, temp);
  masm.leftShiftInt32x4(rsd, temp, rsd);
}

static void ShiftLeftI64x2(MacroAssembler& masm, RegI32 rs, RegV128 rsd,
                           RegI32 temp) {
  ShiftOpMask(masm, SimdOp::I64x2Shl, rs, temp);
  masm.leftShiftInt64x2(rsd, temp, rsd);
}

static void ShiftRightI8x16(MacroAssembler& masm, RegI32 rs, RegV128 rsd,
                            RegI32 temp) {
  ShiftOpMask(masm, SimdOp::I8x16ShrS, rs, temp);
  masm.rightShiftInt8x16(rsd, temp, rsd);
}

static void ShiftRightUI8x16(MacroAssembler& masm, RegI32 rs, RegV128 rsd,
                             RegI32 temp) {
  ShiftOpMask(masm, SimdOp::I8x16ShrU, rs, temp);
  masm.unsignedRightShiftInt8x16(rsd, temp, rsd);
}

static void ShiftRightI16x8(MacroAssembler& masm, RegI32 rs, RegV128 rsd,
                            RegI32 temp) {
  ShiftOpMask(masm, SimdOp::I16x8ShrS, rs, temp);
  masm.rightShiftInt16x8(rsd, temp, rsd);
}

static void ShiftRightUI16x8(MacroAssembler& masm, RegI32 rs, RegV128 rsd,
                             RegI32 temp) {
  ShiftOpMask(masm, SimdOp::I16x8ShrU, rs, temp);
  masm.unsignedRightShiftInt16x8(rsd, temp, rsd);
}

static void ShiftRightI32x4(MacroAssembler& masm, RegI32 rs, RegV128 rsd,
                            RegI32 temp) {
  ShiftOpMask(masm, SimdOp::I32x4ShrS, rs, temp);
  masm.rightShiftInt32x4(rsd, temp, rsd);
}

static void ShiftRightUI32x4(MacroAssembler& masm, RegI32 rs, RegV128 rsd,
                             RegI32 temp) {
  ShiftOpMask(masm, SimdOp::I32x4ShrU, rs, temp);
  masm.unsignedRightShiftInt32x4(rsd, temp, rsd);
}

static void ShiftRightI64x2(MacroAssembler& masm, RegI32 rs, RegV128 rsd,
                            RegI32 temp) {
  ShiftOpMask(masm, SimdOp::I64x2ShrS, rs, temp);
  masm.rightShiftInt64x2(rsd, temp, rsd);
}

static void ShiftRightUI64x2(MacroAssembler& masm, RegI32 rs, RegV128 rsd,
                             RegI32 temp) {
  ShiftOpMask(masm, SimdOp::I64x2ShrU, rs, temp);
  masm.unsignedRightShiftInt64x2(rsd, temp, rsd);
}
#  endif

static void AverageUI8x16(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.unsignedAverageInt8x16(rsd, rs, rsd);
}

static void AverageUI16x8(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.unsignedAverageInt16x8(rsd, rs, rsd);
}

static void MinI8x16(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.minInt8x16(rsd, rs, rsd);
}

static void MinUI8x16(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.unsignedMinInt8x16(rsd, rs, rsd);
}

static void MaxI8x16(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.maxInt8x16(rsd, rs, rsd);
}

static void MaxUI8x16(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.unsignedMaxInt8x16(rsd, rs, rsd);
}

static void MinI16x8(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.minInt16x8(rsd, rs, rsd);
}

static void MinUI16x8(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.unsignedMinInt16x8(rsd, rs, rsd);
}

static void MaxI16x8(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.maxInt16x8(rsd, rs, rsd);
}

static void MaxUI16x8(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.unsignedMaxInt16x8(rsd, rs, rsd);
}

static void MinI32x4(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.minInt32x4(rsd, rs, rsd);
}

static void MinUI32x4(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.unsignedMinInt32x4(rsd, rs, rsd);
}

static void MaxI32x4(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.maxInt32x4(rsd, rs, rsd);
}

static void MaxUI32x4(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.unsignedMaxInt32x4(rsd, rs, rsd);
}

static void NarrowI16x8(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.narrowInt16x8(rsd, rs, rsd);
}

static void NarrowUI16x8(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.unsignedNarrowInt16x8(rsd, rs, rsd);
}

static void NarrowI32x4(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.narrowInt32x4(rsd, rs, rsd);
}

static void NarrowUI32x4(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.unsignedNarrowInt32x4(rsd, rs, rsd);
}

static void WidenLowI8x16(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.widenLowInt8x16(rs, rd);
}

static void WidenHighI8x16(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.widenHighInt8x16(rs, rd);
}

static void WidenLowUI8x16(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.unsignedWidenLowInt8x16(rs, rd);
}

static void WidenHighUI8x16(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.unsignedWidenHighInt8x16(rs, rd);
}

static void WidenLowI16x8(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.widenLowInt16x8(rs, rd);
}

static void WidenHighI16x8(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.widenHighInt16x8(rs, rd);
}

static void WidenLowUI16x8(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.unsignedWidenLowInt16x8(rs, rd);
}

static void WidenHighUI16x8(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.unsignedWidenHighInt16x8(rs, rd);
}

static void WidenLowI32x4(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.widenLowInt32x4(rs, rd);
}

static void WidenHighI32x4(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.widenHighInt32x4(rs, rd);
}

static void WidenLowUI32x4(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.unsignedWidenLowInt32x4(rs, rd);
}

static void WidenHighUI32x4(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.unsignedWidenHighInt32x4(rs, rd);
}

#  if defined(JS_CODEGEN_ARM64)
static void PopcntI8x16(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.popcntInt8x16(rs, rd);
}
#  else
static void PopcntI8x16(MacroAssembler& masm, RegV128 rs, RegV128 rd,
                        RegV128 temp) {
  masm.popcntInt8x16(rs, rd, temp);
}
#  endif  // JS_CODEGEN_ARM64

static void AbsI8x16(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.absInt8x16(rs, rd);
}

static void AbsI16x8(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.absInt16x8(rs, rd);
}

static void AbsI32x4(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.absInt32x4(rs, rd);
}

static void AbsI64x2(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.absInt64x2(rs, rd);
}

static void ExtractLaneI8x16(MacroAssembler& masm, uint32_t laneIndex,
                             RegV128 rs, RegI32 rd) {
  masm.extractLaneInt8x16(laneIndex, rs, rd);
}

static void ExtractLaneUI8x16(MacroAssembler& masm, uint32_t laneIndex,
                              RegV128 rs, RegI32 rd) {
  masm.unsignedExtractLaneInt8x16(laneIndex, rs, rd);
}

static void ExtractLaneI16x8(MacroAssembler& masm, uint32_t laneIndex,
                             RegV128 rs, RegI32 rd) {
  masm.extractLaneInt16x8(laneIndex, rs, rd);
}

static void ExtractLaneUI16x8(MacroAssembler& masm, uint32_t laneIndex,
                              RegV128 rs, RegI32 rd) {
  masm.unsignedExtractLaneInt16x8(laneIndex, rs, rd);
}

static void ExtractLaneI32x4(MacroAssembler& masm, uint32_t laneIndex,
                             RegV128 rs, RegI32 rd) {
  masm.extractLaneInt32x4(laneIndex, rs, rd);
}

static void ExtractLaneI64x2(MacroAssembler& masm, uint32_t laneIndex,
                             RegV128 rs, RegI64 rd) {
  masm.extractLaneInt64x2(laneIndex, rs, rd);
}

static void ExtractLaneF32x4(MacroAssembler& masm, uint32_t laneIndex,
                             RegV128 rs, RegF32 rd) {
  masm.extractLaneFloat32x4(laneIndex, rs, rd);
}

static void ExtractLaneF64x2(MacroAssembler& masm, uint32_t laneIndex,
                             RegV128 rs, RegF64 rd) {
  masm.extractLaneFloat64x2(laneIndex, rs, rd);
}

static void ReplaceLaneI8x16(MacroAssembler& masm, uint32_t laneIndex,
                             RegI32 rs, RegV128 rsd) {
  masm.replaceLaneInt8x16(laneIndex, rs, rsd);
}

static void ReplaceLaneI16x8(MacroAssembler& masm, uint32_t laneIndex,
                             RegI32 rs, RegV128 rsd) {
  masm.replaceLaneInt16x8(laneIndex, rs, rsd);
}

static void ReplaceLaneI32x4(MacroAssembler& masm, uint32_t laneIndex,
                             RegI32 rs, RegV128 rsd) {
  masm.replaceLaneInt32x4(laneIndex, rs, rsd);
}

static void ReplaceLaneI64x2(MacroAssembler& masm, uint32_t laneIndex,
                             RegI64 rs, RegV128 rsd) {
  masm.replaceLaneInt64x2(laneIndex, rs, rsd);
}

static void ReplaceLaneF32x4(MacroAssembler& masm, uint32_t laneIndex,
                             RegF32 rs, RegV128 rsd) {
  masm.replaceLaneFloat32x4(laneIndex, rs, rsd);
}

static void ReplaceLaneF64x2(MacroAssembler& masm, uint32_t laneIndex,
                             RegF64 rs, RegV128 rsd) {
  masm.replaceLaneFloat64x2(laneIndex, rs, rsd);
}

static void SplatI8x16(MacroAssembler& masm, RegI32 rs, RegV128 rd) {
  masm.splatX16(rs, rd);
}

static void SplatI16x8(MacroAssembler& masm, RegI32 rs, RegV128 rd) {
  masm.splatX8(rs, rd);
}

static void SplatI32x4(MacroAssembler& masm, RegI32 rs, RegV128 rd) {
  masm.splatX4(rs, rd);
}

static void SplatI64x2(MacroAssembler& masm, RegI64 rs, RegV128 rd) {
  masm.splatX2(rs, rd);
}

static void SplatF32x4(MacroAssembler& masm, RegF32 rs, RegV128 rd) {
  masm.splatX4(rs, rd);
}

static void SplatF64x2(MacroAssembler& masm, RegF64 rs, RegV128 rd) {
  masm.splatX2(rs, rd);
}

// This is the same op independent of lanes: it tests for any nonzero bit.
static void AnyTrue(MacroAssembler& masm, RegV128 rs, RegI32 rd) {
  masm.anyTrueSimd128(rs, rd);
}

static void AllTrueI8x16(MacroAssembler& masm, RegV128 rs, RegI32 rd) {
  masm.allTrueInt8x16(rs, rd);
}

static void AllTrueI16x8(MacroAssembler& masm, RegV128 rs, RegI32 rd) {
  masm.allTrueInt16x8(rs, rd);
}

static void AllTrueI32x4(MacroAssembler& masm, RegV128 rs, RegI32 rd) {
  masm.allTrueInt32x4(rs, rd);
}

static void AllTrueI64x2(MacroAssembler& masm, RegV128 rs, RegI32 rd) {
  masm.allTrueInt64x2(rs, rd);
}

#  if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
static void BitmaskI8x16(MacroAssembler& masm, RegV128 rs, RegI32 rd) {
  masm.bitmaskInt8x16(rs, rd);
}

static void BitmaskI16x8(MacroAssembler& masm, RegV128 rs, RegI32 rd) {
  masm.bitmaskInt16x8(rs, rd);
}

static void BitmaskI32x4(MacroAssembler& masm, RegV128 rs, RegI32 rd) {
  masm.bitmaskInt32x4(rs, rd);
}

static void BitmaskI64x2(MacroAssembler& masm, RegV128 rs, RegI32 rd) {
  masm.bitmaskInt64x2(rs, rd);
}
#  elif defined(JS_CODEGEN_ARM64)
static void BitmaskI8x16(MacroAssembler& masm, RegV128 rs, RegI32 rd,
                         RegV128 temp) {
  masm.bitmaskInt8x16(rs, rd, temp);
}

static void BitmaskI16x8(MacroAssembler& masm, RegV128 rs, RegI32 rd,
                         RegV128 temp) {
  masm.bitmaskInt16x8(rs, rd, temp);
}

static void BitmaskI32x4(MacroAssembler& masm, RegV128 rs, RegI32 rd,
                         RegV128 temp) {
  masm.bitmaskInt32x4(rs, rd, temp);
}

static void BitmaskI64x2(MacroAssembler& masm, RegV128 rs, RegI32 rd,
                         RegV128 temp) {
  masm.bitmaskInt64x2(rs, rd, temp);
}
#  endif

static void Swizzle(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.swizzleInt8x16(rsd, rs, rsd);
}

static void ConvertI32x4ToF32x4(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.convertInt32x4ToFloat32x4(rs, rd);
}

static void ConvertUI32x4ToF32x4(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.unsignedConvertInt32x4ToFloat32x4(rs, rd);
}

static void ConvertF32x4ToI32x4(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.truncSatFloat32x4ToInt32x4(rs, rd);
}

#  if defined(JS_CODEGEN_ARM64)
static void ConvertF32x4ToUI32x4(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.unsignedTruncSatFloat32x4ToInt32x4(rs, rd);
}
#  else
static void ConvertF32x4ToUI32x4(MacroAssembler& masm, RegV128 rs, RegV128 rd,
                                 RegV128 temp) {
  masm.unsignedTruncSatFloat32x4ToInt32x4(rs, rd, temp);
}
#  endif  // JS_CODEGEN_ARM64

static void ConvertI32x4ToF64x2(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.convertInt32x4ToFloat64x2(rs, rd);
}

static void ConvertUI32x4ToF64x2(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.unsignedConvertInt32x4ToFloat64x2(rs, rd);
}

static void ConvertF64x2ToI32x4(MacroAssembler& masm, RegV128 rs, RegV128 rd,
                                RegV128 temp) {
  masm.truncSatFloat64x2ToInt32x4(rs, rd, temp);
}

static void ConvertF64x2ToUI32x4(MacroAssembler& masm, RegV128 rs, RegV128 rd,
                                 RegV128 temp) {
  masm.unsignedTruncSatFloat64x2ToInt32x4(rs, rd, temp);
}

static void DemoteF64x2ToF32x4(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.convertFloat64x2ToFloat32x4(rs, rd);
}

static void PromoteF32x4ToF64x2(MacroAssembler& masm, RegV128 rs, RegV128 rd) {
  masm.convertFloat32x4ToFloat64x2(rs, rd);
}

#  if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
static void BitselectV128(MacroAssembler& masm, RegV128 rhs, RegV128 control,
                          RegV128 lhsDest, RegV128 temp) {
  // Ideally, we would have temp=control, and we can probably get away with
  // just doing that, but don't worry about it yet.
  masm.bitwiseSelectSimd128(control, lhsDest, rhs, lhsDest, temp);
}
#  elif defined(JS_CODEGEN_ARM64)
static void BitselectV128(MacroAssembler& masm, RegV128 rhs, RegV128 control,
                          RegV128 lhsDest, RegV128 temp) {
  // The masm interface is not great for the baseline compiler here, but it's
  // optimal for Ion, so just work around it.
  masm.moveSimd128(control, temp);
  masm.bitwiseSelectSimd128(lhsDest, rhs, temp);
  masm.moveSimd128(temp, lhsDest);
}
#  endif

#  ifdef ENABLE_WASM_RELAXED_SIMD
static void RelaxedMaddF32x4(MacroAssembler& masm, RegV128 rs1, RegV128 rs2,
                             RegV128 rsd) {
  masm.fmaFloat32x4(rs1, rs2, rsd);
}

static void RelaxedNmaddF32x4(MacroAssembler& masm, RegV128 rs1, RegV128 rs2,
                              RegV128 rsd) {
  masm.fnmaFloat32x4(rs1, rs2, rsd);
}

static void RelaxedMaddF64x2(MacroAssembler& masm, RegV128 rs1, RegV128 rs2,
                             RegV128 rsd) {
  masm.fmaFloat64x2(rs1, rs2, rsd);
}

static void RelaxedNmaddF64x2(MacroAssembler& masm, RegV128 rs1, RegV128 rs2,
                              RegV128 rsd) {
  masm.fnmaFloat64x2(rs1, rs2, rsd);
}

static void RelaxedSwizzle(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.swizzleInt8x16Relaxed(rsd, rs, rsd);
}

static void RelaxedMinF32x4(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.minFloat32x4Relaxed(rs, rsd);
}

static void RelaxedMaxF32x4(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.maxFloat32x4Relaxed(rs, rsd);
}

static void RelaxedMinF64x2(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.minFloat64x2Relaxed(rs, rsd);
}

static void RelaxedMaxF64x2(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.maxFloat64x2Relaxed(rs, rsd);
}

static void RelaxedConvertF32x4ToI32x4(MacroAssembler& masm, RegV128 rs,
                                       RegV128 rd) {
  masm.truncFloat32x4ToInt32x4Relaxed(rs, rd);
}

static void RelaxedConvertF32x4ToUI32x4(MacroAssembler& masm, RegV128 rs,
                                        RegV128 rd) {
  masm.unsignedTruncFloat32x4ToInt32x4Relaxed(rs, rd);
}

static void RelaxedConvertF64x2ToI32x4(MacroAssembler& masm, RegV128 rs,
                                       RegV128 rd) {
  masm.truncFloat64x2ToInt32x4Relaxed(rs, rd);
}

static void RelaxedConvertF64x2ToUI32x4(MacroAssembler& masm, RegV128 rs,
                                        RegV128 rd) {
  masm.unsignedTruncFloat64x2ToInt32x4Relaxed(rs, rd);
}

static void RelaxedQ15MulrS(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.q15MulrInt16x8Relaxed(rsd, rs, rsd);
}

static void DotI8x16I7x16S(MacroAssembler& masm, RegV128 rs, RegV128 rsd) {
  masm.dotInt8x16Int7x16(rsd, rs, rsd);
}

void BaseCompiler::emitDotI8x16I7x16AddS() {
  RegV128 rsd = popV128();
  RegV128 rs0, rs1;
  pop2xV128(&rs0, &rs1);
#    if defined(JS_CODEGEN_ARM64)
  RegV128 temp = needV128();
  masm.dotInt8x16Int7x16ThenAdd(rs0, rs1, rsd, temp);
  freeV128(temp);
#    else
  masm.dotInt8x16Int7x16ThenAdd(rs0, rs1, rsd);
#    endif
  freeV128(rs1);
  freeV128(rs0);
  pushV128(rsd);
}
#  endif  // ENABLE_WASM_RELAXED_SIMD

void BaseCompiler::emitVectorAndNot() {
  // We want x & ~y but the available operation is ~x & y, so reverse the
  // operands.
  RegV128 r, rs;
  pop2xV128(&r, &rs);
  masm.bitwiseNotAndSimd128(r, rs);
  freeV128(r);
  pushV128(rs);
}

bool BaseCompiler::emitLoadSplat(Scalar::Type viewType) {
  LinearMemoryAddress<Nothing> addr;
  if (!iter_.readLoadSplat(Scalar::byteSize(viewType), &addr)) {
    return false;
  }
  if (deadCode_) {
    return true;
  }
  MemoryAccessDesc access(addr.memoryIndex, viewType, addr.align, addr.offset,
                          trapSiteDesc(), hugeMemoryEnabled(addr.memoryIndex));
  loadSplat(&access);
  return true;
}

bool BaseCompiler::emitLoadZero(Scalar::Type viewType) {
  // LoadZero has the structure of LoadSplat, so reuse the reader.
  LinearMemoryAddress<Nothing> addr;
  if (!iter_.readLoadSplat(Scalar::byteSize(viewType), &addr)) {
    return false;
  }
  if (deadCode_) {
    return true;
  }
  MemoryAccessDesc access(addr.memoryIndex, viewType, addr.align, addr.offset,
                          trapSiteDesc(), hugeMemoryEnabled(addr.memoryIndex));
  loadZero(&access);
  return true;
}

bool BaseCompiler::emitLoadExtend(Scalar::Type viewType) {
  LinearMemoryAddress<Nothing> addr;
  if (!iter_.readLoadExtend(&addr)) {
    return false;
  }
  if (deadCode_) {
    return true;
  }
  MemoryAccessDesc access(addr.memoryIndex, Scalar::Int64, addr.align,
                          addr.offset, trapSiteDesc(),
                          hugeMemoryEnabled(addr.memoryIndex));
  loadExtend(&access, viewType);
  return true;
}

bool BaseCompiler::emitLoadLane(uint32_t laneSize) {
  Nothing nothing;
  LinearMemoryAddress<Nothing> addr;
  uint32_t laneIndex;
  if (!iter_.readLoadLane(laneSize, &addr, &laneIndex, &nothing)) {
    return false;
  }
  if (deadCode_) {
    return true;
  }
  Scalar::Type viewType;
  switch (laneSize) {
    case 1:
      viewType = Scalar::Uint8;
      break;
    case 2:
      viewType = Scalar::Uint16;
      break;
    case 4:
      viewType = Scalar::Int32;
      break;
    case 8:
      viewType = Scalar::Int64;
      break;
    default:
      MOZ_CRASH("unsupported laneSize");
  }
  MemoryAccessDesc access(addr.memoryIndex, viewType, addr.align, addr.offset,
                          trapSiteDesc(), hugeMemoryEnabled(addr.memoryIndex));
  loadLane(&access, laneIndex);
  return true;
}

bool BaseCompiler::emitStoreLane(uint32_t laneSize) {
  Nothing nothing;
  LinearMemoryAddress<Nothing> addr;
  uint32_t laneIndex;
  if (!iter_.readStoreLane(laneSize, &addr, &laneIndex, &nothing)) {
    return false;
  }
  if (deadCode_) {
    return true;
  }
  Scalar::Type viewType;
  switch (laneSize) {
    case 1:
      viewType = Scalar::Uint8;
      break;
    case 2:
      viewType = Scalar::Uint16;
      break;
    case 4:
      viewType = Scalar::Int32;
      break;
    case 8:
      viewType = Scalar::Int64;
      break;
    default:
      MOZ_CRASH("unsupported laneSize");
  }
  MemoryAccessDesc access(addr.memoryIndex, viewType, addr.align, addr.offset,
                          trapSiteDesc(), hugeMemoryEnabled(addr.memoryIndex));
  storeLane(&access, laneIndex);
  return true;
}

bool BaseCompiler::emitVectorShuffle() {
  Nothing unused_a, unused_b;
  V128 shuffleMask;

  if (!iter_.readVectorShuffle(&unused_a, &unused_b, &shuffleMask)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  RegV128 rd, rs;
  pop2xV128(&rd, &rs);

  masm.shuffleInt8x16(shuffleMask.bytes, rs, rd);

  freeV128(rs);
  pushV128(rd);

  return true;
}

#  if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
bool BaseCompiler::emitVectorShiftRightI64x2() {
  Nothing unused_a, unused_b;

  if (!iter_.readVectorShift(&unused_a, &unused_b)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

  RegI32 count = popI32RhsForShiftI64();
  RegV128 lhsDest = popV128();
  RegI64 tmp = needI64();
  masm.and32(Imm32(63), count);
  masm.extractLaneInt64x2(0, lhsDest, tmp);
  masm.rshift64Arithmetic(count, tmp);
  masm.replaceLaneInt64x2(0, tmp, lhsDest);
  masm.extractLaneInt64x2(1, lhsDest, tmp);
  masm.rshift64Arithmetic(count, tmp);
  masm.replaceLaneInt64x2(1, tmp, lhsDest);
  freeI64(tmp);
  freeI32(count);
  pushV128(lhsDest);

  return true;
}
#  endif

#  ifdef ENABLE_WASM_RELAXED_SIMD
bool BaseCompiler::emitVectorLaneSelect() {
  Nothing unused_a, unused_b, unused_c;

  if (!iter_.readTernary(ValType::V128, &unused_a, &unused_b, &unused_c)) {
    return false;
  }

  if (deadCode_) {
    return true;
  }

#    if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
  RegV128 mask = popV128(RegV128(vmm0));
  RegV128 rhsDest = popV128();
  RegV128 lhs = popV128();
  masm.laneSelectSimd128(mask, lhs, rhsDest, rhsDest);
  freeV128(lhs);
  freeV128(mask);
  pushV128(rhsDest);
#    elif defined(JS_CODEGEN_ARM64)
  RegV128 maskDest = popV128();
  RegV128 rhs = popV128();
  RegV128 lhs = popV128();
  masm.laneSelectSimd128(maskDest, lhs, rhs, maskDest);
  freeV128(lhs);
  freeV128(rhs);
  pushV128(maskDest);
#    endif

  return true;
}
#  endif  // ENABLE_WASM_RELAXED_SIMD
#endif    // ENABLE_WASM_SIMD


//////////////////////////////////////////////////////////////////////////////
//
// Function bodies - main opcode dispatch loop.

bool BaseCompiler::beginPushedBody() {
  MOZ_ASSERT(stackMapGenerator_.framePushedAtEntryToBody.isSome());
  MOZ_ASSERT(controlStack_.empty());

  if (!pushControl(LabelKind::Body, BlockType::FuncResults(funcType()))) {
    return false;
  }

  initControl(controlItem(), ResultType::Empty());
  return true;
}

bool BaseCompiler::beginPushedOp(OpBytes op, size_t opOffset) {
  AutoCreatedBy acb(masm, "(wasm)BaseCompiler::beginPushedOp");

#ifdef DEBUG
  performRegisterLeakCheck();
  assertStackInvariants();
#endif

  // Opcodes that push more than MaxPushesPerOpcode (anything with multiple
  // results) will perform additional reservation.
  if (!stk_.reserve(stk_.length() + MaxPushesPerOpcode)) {
    return false;
  }

  // There is no readOp to record this: the validator read the operator, and
  // where it started is what trap sites and the perf spewer want.
  currentOpOffset_ = opOffset;

#ifdef JS_ION_PERF
  bool spewerEnabled = perfSpewer_.needsToRecordInstruction();
#else
  bool spewerEnabled = false;
#endif
  bool debugEnabled = compilerEnv_.debugEnabled();

  if (MOZ_UNLIKELY(spewerEnabled || debugEnabled)) {
    // When compilerEnv_.debugEnabled(), some operators get a breakpoint site.
    if (debugEnabled && op.shouldHaveBreakpoint() && !deadCode_) {
      if (previousBreakablePoint_ != masm.currentOffset()) {
        sync();

        insertBreakablePoint(CallSiteKind::Breakpoint);
        // stackmap for: debug: per-insn breakpoint
        if (!createStackMap(Nothing() /* stackmap pertains to a call */)) {
          return false;
        }
        previousBreakablePoint_ = masm.currentOffset();
      }
    }

#ifdef JS_ION_PERF
    if (spewerEnabled) {
      perfSpewer_.recordInstruction(masm, op);
    }
#endif
  }

  // Going below framePushedAtEntryToBody would imply that we've popped off the
  // machine stack, part of the frame created by beginFunction().
  MOZ_ASSERT(masm.framePushed() >=
             stackMapGenerator_.framePushedAtEntryToBody.value());

  // At this point we're definitely not generating code for a function call.
  MOZ_ASSERT(
      stackMapGenerator_.framePushedExcludingOutboundCallArgs.isNothing());

  return true;
}

void BaseCompiler::endPushedOp() {
#ifdef DEBUG
  // Check that the number of ref-typed entries in the operand stack matches
  // reality.
  MOZ_ASSERT(countMemRefsOnStk() == stackMapGenerator_.memRefsOnStk);
#endif
}

//////////////////////////////////////////////////////////////////////////////
//
// One method per numeric operator, and the four constants and two
// zero-immediate control operators with no existing named emitter.
//
// Each hook calls one of these directly: there is no switch behind the call,
// so a hook cannot reach the wrong one.

// The dispatch macros the generated methods below use: each is the codegen
// for one shape of numeric operator, skipping it while in dead code. `type`
// (or `inType`/`outType`/`operandType`) is unused in the macro body: it named
// the operand type OpIter's redundant, cross-checking validation wanted, and
// stays as a parameter only so every call site below, generated from the
// opcode's real operand type, does not need editing.
#define dispatchBinary0(doEmit, type) (deadCode_ || (doEmit(), true))

#define dispatchBinary1(arg1, type) (deadCode_ || (emitBinop(arg1), true))

#define dispatchBinary2(arg1, arg2, type) \
  (deadCode_ || (emitBinop(arg1, arg2), true))

#define dispatchBinary3(arg1, arg2, arg3, type) \
  (deadCode_ || (emitBinop(arg1, arg2, arg3), true))

#define dispatchUnary1(arg1, type) (deadCode_ || (emitUnop(arg1), true))

#define dispatchUnary2(arg1, arg2, type) \
  (deadCode_ || (emitUnop(arg1, arg2), true))

#define dispatchComparison0(doEmit, operandType, compareOp) \
  (deadCode_ || (doEmit(compareOp, operandType), true))

#define dispatchConversion0(doEmit, inType, outType) \
  (deadCode_ || (doEmit(), true))

#define dispatchConversion1(arg1, inType, outType) \
  (deadCode_ || (emitUnop(arg1), true))

#define dispatchConversionOOM(doEmit, inType, outType) (deadCode_ || doEmit())

#define dispatchCalloutConversionOOM(doEmit, symbol, inType, outType) \
  (deadCode_ || doEmit(symbol, inType, outType))

#define dispatchIntDivCallout(doEmit, symbol, type) \
  (deadCode_ || doEmit(symbol, type))

bool BaseCompiler::emitOpI32Eqz() {
  return dispatchConversion0(emitEqzI32, ValType::I32, ValType::I32);
}

bool BaseCompiler::emitOpI32Eq() {
  return dispatchComparison0(emitCompareI32, ValType::I32,
                                 Assembler::Equal);
}

bool BaseCompiler::emitOpI32Ne() {
  return dispatchComparison0(emitCompareI32, ValType::I32,
                                 Assembler::NotEqual);
}

bool BaseCompiler::emitOpI32LtS() {
  return dispatchComparison0(emitCompareI32, ValType::I32,
                                 Assembler::LessThan);
}

bool BaseCompiler::emitOpI32LtU() {
  return dispatchComparison0(emitCompareI32, ValType::I32,
                                 Assembler::Below);
}

bool BaseCompiler::emitOpI32GtS() {
  return dispatchComparison0(emitCompareI32, ValType::I32,
                                 Assembler::GreaterThan);
}

bool BaseCompiler::emitOpI32GtU() {
  return dispatchComparison0(emitCompareI32, ValType::I32,
                                 Assembler::Above);
}

bool BaseCompiler::emitOpI32LeS() {
  return dispatchComparison0(emitCompareI32, ValType::I32,
                                 Assembler::LessThanOrEqual);
}

bool BaseCompiler::emitOpI32LeU() {
  return dispatchComparison0(emitCompareI32, ValType::I32,
                                 Assembler::BelowOrEqual);
}

bool BaseCompiler::emitOpI32GeS() {
  return dispatchComparison0(emitCompareI32, ValType::I32,
                                 Assembler::GreaterThanOrEqual);
}

bool BaseCompiler::emitOpI32GeU() {
  return dispatchComparison0(emitCompareI32, ValType::I32,
                                 Assembler::AboveOrEqual);
}

bool BaseCompiler::emitOpI64Eqz() {
  return dispatchConversion0(emitEqzI64, ValType::I64, ValType::I32);
}

bool BaseCompiler::emitOpI64Eq() {
  return dispatchComparison0(emitCompareI64, ValType::I64,
                                 Assembler::Equal);
}

bool BaseCompiler::emitOpI64Ne() {
  return dispatchComparison0(emitCompareI64, ValType::I64,
                                 Assembler::NotEqual);
}

bool BaseCompiler::emitOpI64LtS() {
  return dispatchComparison0(emitCompareI64, ValType::I64,
                                 Assembler::LessThan);
}

bool BaseCompiler::emitOpI64LtU() {
  return dispatchComparison0(emitCompareI64, ValType::I64,
                                 Assembler::Below);
}

bool BaseCompiler::emitOpI64GtS() {
  return dispatchComparison0(emitCompareI64, ValType::I64,
                                 Assembler::GreaterThan);
}

bool BaseCompiler::emitOpI64GtU() {
  return dispatchComparison0(emitCompareI64, ValType::I64,
                                 Assembler::Above);
}

bool BaseCompiler::emitOpI64LeS() {
  return dispatchComparison0(emitCompareI64, ValType::I64,
                                 Assembler::LessThanOrEqual);
}

bool BaseCompiler::emitOpI64LeU() {
  return dispatchComparison0(emitCompareI64, ValType::I64,
                                 Assembler::BelowOrEqual);
}

bool BaseCompiler::emitOpI64GeS() {
  return dispatchComparison0(emitCompareI64, ValType::I64,
                                 Assembler::GreaterThanOrEqual);
}

bool BaseCompiler::emitOpI64GeU() {
  return dispatchComparison0(emitCompareI64, ValType::I64,
                                 Assembler::AboveOrEqual);
}

bool BaseCompiler::emitOpF32Eq() {
  return dispatchComparison0(emitCompareF32, ValType::F32,
                                 Assembler::DoubleEqual);
}

bool BaseCompiler::emitOpF32Ne() {
  return dispatchComparison0(emitCompareF32, ValType::F32,
                                 Assembler::DoubleNotEqualOrUnordered);
}

bool BaseCompiler::emitOpF32Lt() {
  return dispatchComparison0(emitCompareF32, ValType::F32,
                                 Assembler::DoubleLessThan);
}

bool BaseCompiler::emitOpF32Gt() {
  return dispatchComparison0(emitCompareF32, ValType::F32,
                                 Assembler::DoubleGreaterThan);
}

bool BaseCompiler::emitOpF32Le() {
  return dispatchComparison0(emitCompareF32, ValType::F32,
                                 Assembler::DoubleLessThanOrEqual);
}

bool BaseCompiler::emitOpF32Ge() {
  return dispatchComparison0(emitCompareF32, ValType::F32,
                                 Assembler::DoubleGreaterThanOrEqual);
}

bool BaseCompiler::emitOpF64Eq() {
  return dispatchComparison0(emitCompareF64, ValType::F64,
                                 Assembler::DoubleEqual);
}

bool BaseCompiler::emitOpF64Ne() {
  return dispatchComparison0(emitCompareF64, ValType::F64,
                                 Assembler::DoubleNotEqualOrUnordered);
}

bool BaseCompiler::emitOpF64Lt() {
  return dispatchComparison0(emitCompareF64, ValType::F64,
                                 Assembler::DoubleLessThan);
}

bool BaseCompiler::emitOpF64Gt() {
  return dispatchComparison0(emitCompareF64, ValType::F64,
                                 Assembler::DoubleGreaterThan);
}

bool BaseCompiler::emitOpF64Le() {
  return dispatchComparison0(emitCompareF64, ValType::F64,
                                 Assembler::DoubleLessThanOrEqual);
}

bool BaseCompiler::emitOpF64Ge() {
  return dispatchComparison0(emitCompareF64, ValType::F64,
                                 Assembler::DoubleGreaterThanOrEqual);
  // Memory Related
}

bool BaseCompiler::emitOpI32Sub() {
  return dispatchBinary2(SubI32, SubImmI32, ValType::I32);
}

bool BaseCompiler::emitOpI32Mul() {
  return dispatchBinary1(MulI32, ValType::I32);
}

bool BaseCompiler::emitOpI32DivS() {
  return dispatchBinary0(emitQuotientI32, ValType::I32);
}

bool BaseCompiler::emitOpI32DivU() {
  return dispatchBinary0(emitQuotientU32, ValType::I32);
}

bool BaseCompiler::emitOpI32RemS() {
  return dispatchBinary0(emitRemainderI32, ValType::I32);
}

bool BaseCompiler::emitOpI32RemU() {
  return dispatchBinary0(emitRemainderU32, ValType::I32);
}

bool BaseCompiler::emitOpI32And() {
  return dispatchBinary2(AndI32, AndImmI32, ValType::I32);
}

bool BaseCompiler::emitOpI32Or() {
  return dispatchBinary2(OrI32, OrImmI32, ValType::I32);
}

bool BaseCompiler::emitOpI32Xor() {
  return dispatchBinary2(XorI32, XorImmI32, ValType::I32);
}

bool BaseCompiler::emitOpI32Shl() {
  return dispatchBinary3(
      ShlI32, ShlImmI32, &BaseCompiler::popI32RhsForShift, ValType::I32);
}

bool BaseCompiler::emitOpI32ShrS() {
  return dispatchBinary3(
      ShrI32, ShrImmI32, &BaseCompiler::popI32RhsForShift, ValType::I32);
}

bool BaseCompiler::emitOpI32ShrU() {
  return dispatchBinary3(ShrUI32, ShrUImmI32,
                             &BaseCompiler::popI32RhsForShift,
                             ValType::I32);
}

bool BaseCompiler::emitOpI32Rotl() {
  return dispatchBinary3(RotlI32, RotlImmI32,
                             &BaseCompiler::popI32RhsForRotate,
                             ValType::I32);
}

bool BaseCompiler::emitOpI32Rotr() {
  return dispatchBinary3(RotrI32, RotrImmI32,
                             &BaseCompiler::popI32RhsForRotate,
                             ValType::I32);
}

bool BaseCompiler::emitOpI64Add() {
  return dispatchBinary2(AddI64, AddImmI64, ValType::I64);
}

bool BaseCompiler::emitOpI64Sub() {
  return dispatchBinary2(SubI64, SubImmI64, ValType::I64);
}

bool BaseCompiler::emitOpI64Mul() {
  return dispatchBinary0(emitMultiplyI64, ValType::I64);
}

bool BaseCompiler::emitOpI64DivS() {
#ifdef RABALDR_INT_DIV_I64_CALLOUT
  return dispatchIntDivCallout(
      emitDivOrModI64BuiltinCall, SymbolicAddress::DivI64, ValType::I64);
#else
  return dispatchBinary0(emitQuotientI64, ValType::I64);
#endif
}

bool BaseCompiler::emitOpI64DivU() {
#ifdef RABALDR_INT_DIV_I64_CALLOUT
  return dispatchIntDivCallout(emitDivOrModI64BuiltinCall,
                                   SymbolicAddress::UDivI64,
                                   ValType::I64);
#else
  return dispatchBinary0(emitQuotientU64, ValType::I64);
#endif
}

bool BaseCompiler::emitOpI64RemS() {
#ifdef RABALDR_INT_DIV_I64_CALLOUT
  return dispatchIntDivCallout(
      emitDivOrModI64BuiltinCall, SymbolicAddress::ModI64, ValType::I64);
#else
  return dispatchBinary0(emitRemainderI64, ValType::I64);
#endif
}

bool BaseCompiler::emitOpI64RemU() {
#ifdef RABALDR_INT_DIV_I64_CALLOUT
  return dispatchIntDivCallout(emitDivOrModI64BuiltinCall,
                                   SymbolicAddress::UModI64,
                                   ValType::I64);
#else
  return dispatchBinary0(emitRemainderU64, ValType::I64);
#endif
}

bool BaseCompiler::emitOpI64And() {
  return dispatchBinary2(AndI64, AndImmI64, ValType::I64);
}

bool BaseCompiler::emitOpI64Or() {
  return dispatchBinary2(OrI64, OrImmI64, ValType::I64);
}

bool BaseCompiler::emitOpI64Xor() {
  return dispatchBinary2(XorI64, XorImmI64, ValType::I64);
}

bool BaseCompiler::emitOpI64Shl() {
  return dispatchBinary3(
      ShlI64, ShlImmI64, &BaseCompiler::popI64RhsForShift, ValType::I64);
}

bool BaseCompiler::emitOpI64ShrS() {
  return dispatchBinary3(
      ShrI64, ShrImmI64, &BaseCompiler::popI64RhsForShift, ValType::I64);
}

bool BaseCompiler::emitOpI64ShrU() {
  return dispatchBinary3(ShrUI64, ShrUImmI64,
                             &BaseCompiler::popI64RhsForShift,
                             ValType::I64);
}

bool BaseCompiler::emitOpI64Rotl() {
  return dispatchBinary0(emitRotlI64, ValType::I64);
}

bool BaseCompiler::emitOpI64Rotr() {
  return dispatchBinary0(emitRotrI64, ValType::I64);
}

bool BaseCompiler::emitOpF32Add() {
  return dispatchBinary1(AddF32, ValType::F32);
}

bool BaseCompiler::emitOpF32Sub() {
  return dispatchBinary1(SubF32, ValType::F32);
}

bool BaseCompiler::emitOpF32Mul() {
  return dispatchBinary1(MulF32, ValType::F32);
}

bool BaseCompiler::emitOpF32Div() {
  return dispatchBinary1(DivF32, ValType::F32);
}

bool BaseCompiler::emitOpF32Min() {
  return dispatchBinary1(MinF32, ValType::F32);
}

bool BaseCompiler::emitOpF32Max() {
  return dispatchBinary1(MaxF32, ValType::F32);
}

bool BaseCompiler::emitOpF32CopySign() {
  return dispatchBinary1(CopysignF32, ValType::F32);
}

bool BaseCompiler::emitOpF64Add() {
  return dispatchBinary1(AddF64, ValType::F64);
}

bool BaseCompiler::emitOpF64Sub() {
  return dispatchBinary1(SubF64, ValType::F64);
}

bool BaseCompiler::emitOpF64Mul() {
  return dispatchBinary1(MulF64, ValType::F64);
}

bool BaseCompiler::emitOpF64Div() {
  return dispatchBinary1(DivF64, ValType::F64);
}

bool BaseCompiler::emitOpF64Min() {
  return dispatchBinary1(MinF64, ValType::F64);
}

bool BaseCompiler::emitOpF64Max() {
  return dispatchBinary1(MaxF64, ValType::F64);
}

bool BaseCompiler::emitOpF64CopySign() {
  return dispatchBinary1(CopysignF64, ValType::F64);
}

bool BaseCompiler::emitOpI32Clz() {
  return dispatchUnary1(ClzI32, ValType::I32);
}

bool BaseCompiler::emitOpI32Ctz() {
  return dispatchUnary1(CtzI32, ValType::I32);
}

bool BaseCompiler::emitOpI32Popcnt() {
  return dispatchUnary2(PopcntI32, PopcntTemp, ValType::I32);
}

bool BaseCompiler::emitOpI32Add() {
  return dispatchBinary2(AddI32, AddImmI32, ValType::I32);
}

bool BaseCompiler::emitOpI64Clz() {
  return dispatchUnary1(ClzI64, ValType::I64);
}

bool BaseCompiler::emitOpI64Ctz() {
  return dispatchUnary1(CtzI64, ValType::I64);
}

bool BaseCompiler::emitOpI64Popcnt() {
  return dispatchUnary2(PopcntI64, PopcntTemp, ValType::I64);
}

bool BaseCompiler::emitOpF32Abs() {
  return dispatchUnary1(AbsF32, ValType::F32);
}

bool BaseCompiler::emitOpF32Neg() {
  return dispatchUnary1(NegateF32, ValType::F32);
}

bool BaseCompiler::emitOpF32Ceil() {
  return 
      emitUnaryMathBuiltinCall(SymbolicAddress::CeilF, ValType::F32);
}

bool BaseCompiler::emitOpF32Floor() {
  return 
      emitUnaryMathBuiltinCall(SymbolicAddress::FloorF, ValType::F32);
}

bool BaseCompiler::emitOpF32Trunc() {
  return emitUnaryMathBuiltinCall(SymbolicAddress::TruncF, ValType::F32);
}

bool BaseCompiler::emitOpF32Nearest() {
  return emitUnaryMathBuiltinCall(SymbolicAddress::NearbyIntF,
                                      ValType::F32);
}

bool BaseCompiler::emitOpF32Sqrt() {
  return dispatchUnary1(SqrtF32, ValType::F32);
}

bool BaseCompiler::emitOpF64Abs() {
  return dispatchUnary1(AbsF64, ValType::F64);
}

bool BaseCompiler::emitOpF64Neg() {
  return dispatchUnary1(NegateF64, ValType::F64);
}

bool BaseCompiler::emitOpF64Ceil() {
  return 
      emitUnaryMathBuiltinCall(SymbolicAddress::CeilD, ValType::F64);
}

bool BaseCompiler::emitOpF64Floor() {
  return 
      emitUnaryMathBuiltinCall(SymbolicAddress::FloorD, ValType::F64);
}

bool BaseCompiler::emitOpF64Trunc() {
  return 
      emitUnaryMathBuiltinCall(SymbolicAddress::TruncD, ValType::F64);
  // Comparisons
}

bool BaseCompiler::emitOpF64Nearest() {
  return emitUnaryMathBuiltinCall(SymbolicAddress::NearbyIntD,
                                      ValType::F64);
}

bool BaseCompiler::emitOpF64Sqrt() {
  return dispatchUnary1(SqrtF64, ValType::F64);
}

bool BaseCompiler::emitOpI32WrapI64() {
  return 
      dispatchConversion1(WrapI64ToI32, ValType::I64, ValType::I32);
}

bool BaseCompiler::emitOpI32TruncF32S() {
  return dispatchConversionOOM(emitTruncateF32ToI32<0>, ValType::F32,
                                   ValType::I32);
}

bool BaseCompiler::emitOpI32TruncF32U() {
  return dispatchConversionOOM(emitTruncateF32ToI32<TRUNC_UNSIGNED>,
                                   ValType::F32, ValType::I32);
}

bool BaseCompiler::emitOpI32TruncF64S() {
  return dispatchConversionOOM(emitTruncateF64ToI32<0>, ValType::F64,
                                   ValType::I32);
}

bool BaseCompiler::emitOpI32TruncF64U() {
  return dispatchConversionOOM(emitTruncateF64ToI32<TRUNC_UNSIGNED>,
                                   ValType::F64, ValType::I32);
}

bool BaseCompiler::emitOpI64ExtendI32S() {
  return dispatchConversion0(emitExtendI32ToI64, ValType::I32,
                                 ValType::I64);
}

bool BaseCompiler::emitOpI64ExtendI32U() {
  return dispatchConversion0(emitExtendU32ToI64, ValType::I32,
                                 ValType::I64);
}

bool BaseCompiler::emitOpI64TruncF32S() {
#ifdef RABALDR_FLOAT_TO_I64_CALLOUT
  return 
      dispatchCalloutConversionOOM(emitConvertFloatingToInt64Callout,
                                   SymbolicAddress::TruncateDoubleToInt64,
                                   ValType::F32, ValType::I64);
#else
  return dispatchConversionOOM(emitTruncateF32ToI64<0>, ValType::F32,
                                   ValType::I64);
#endif
}

bool BaseCompiler::emitOpI64TruncF32U() {
#ifdef RABALDR_FLOAT_TO_I64_CALLOUT
  return dispatchCalloutConversionOOM(
      emitConvertFloatingToInt64Callout,
      SymbolicAddress::TruncateDoubleToUint64, ValType::F32,
      ValType::I64);
#else
  return dispatchConversionOOM(emitTruncateF32ToI64<TRUNC_UNSIGNED>,
                                   ValType::F32, ValType::I64);
#endif
}

bool BaseCompiler::emitOpI64TruncF64S() {
#ifdef RABALDR_FLOAT_TO_I64_CALLOUT
  return 
      dispatchCalloutConversionOOM(emitConvertFloatingToInt64Callout,
                                   SymbolicAddress::TruncateDoubleToInt64,
                                   ValType::F64, ValType::I64);
#else
  return dispatchConversionOOM(emitTruncateF64ToI64<0>, ValType::F64,
                                   ValType::I64);
#endif
}

bool BaseCompiler::emitOpI64TruncF64U() {
#ifdef RABALDR_FLOAT_TO_I64_CALLOUT
  return dispatchCalloutConversionOOM(
      emitConvertFloatingToInt64Callout,
      SymbolicAddress::TruncateDoubleToUint64, ValType::F64,
      ValType::I64);
#else
  return dispatchConversionOOM(emitTruncateF64ToI64<TRUNC_UNSIGNED>,
                                   ValType::F64, ValType::I64);
#endif
}

bool BaseCompiler::emitOpF32ConvertI32S() {
  return 
      dispatchConversion1(ConvertI32ToF32, ValType::I32, ValType::F32);
}

bool BaseCompiler::emitOpF32ConvertI32U() {
  return 
      dispatchConversion1(ConvertU32ToF32, ValType::I32, ValType::F32);
}

bool BaseCompiler::emitOpF32ConvertI64S() {
#ifdef RABALDR_I64_TO_FLOAT_CALLOUT
  return dispatchCalloutConversionOOM(
      emitConvertInt64ToFloatingCallout, SymbolicAddress::Int64ToFloat32,
      ValType::I64, ValType::F32);
#else
  return 
      dispatchConversion1(ConvertI64ToF32, ValType::I64, ValType::F32);
#endif
}

bool BaseCompiler::emitOpF32ConvertI64U() {
#ifdef RABALDR_I64_TO_FLOAT_CALLOUT
  return dispatchCalloutConversionOOM(
      emitConvertInt64ToFloatingCallout, SymbolicAddress::Uint64ToFloat32,
      ValType::I64, ValType::F32);
#else
  return dispatchConversion0(emitConvertU64ToF32, ValType::I64,
                                 ValType::F32);
#endif
}

bool BaseCompiler::emitOpF32DemoteF64() {
  return 
      dispatchConversion1(ConvertF64ToF32, ValType::F64, ValType::F32);
}

bool BaseCompiler::emitOpF64ConvertI32S() {
  return 
      dispatchConversion1(ConvertI32ToF64, ValType::I32, ValType::F64);
}

bool BaseCompiler::emitOpF64ConvertI32U() {
  return 
      dispatchConversion1(ConvertU32ToF64, ValType::I32, ValType::F64);
}

bool BaseCompiler::emitOpF64ConvertI64S() {
#ifdef RABALDR_I64_TO_FLOAT_CALLOUT
  return dispatchCalloutConversionOOM(
      emitConvertInt64ToFloatingCallout, SymbolicAddress::Int64ToDouble,
      ValType::I64, ValType::F64);
#else
  return 
      dispatchConversion1(ConvertI64ToF64, ValType::I64, ValType::F64);
#endif
}

bool BaseCompiler::emitOpF64ConvertI64U() {
#ifdef RABALDR_I64_TO_FLOAT_CALLOUT
  return dispatchCalloutConversionOOM(
      emitConvertInt64ToFloatingCallout, SymbolicAddress::Uint64ToDouble,
      ValType::I64, ValType::F64);
#else
  return dispatchConversion0(emitConvertU64ToF64, ValType::I64,
                                 ValType::F64);
#endif
}

bool BaseCompiler::emitOpF64PromoteF32() {
  return 
      dispatchConversion1(ConvertF32ToF64, ValType::F32, ValType::F64);
}

bool BaseCompiler::emitOpI32ReinterpretF32() {
  return dispatchConversion1(ReinterpretF32AsI32, ValType::F32,
                                 ValType::I32);
}

bool BaseCompiler::emitOpI64ReinterpretF64() {
  return dispatchConversion1(ReinterpretF64AsI64, ValType::F64,
                                 ValType::I64);
}

bool BaseCompiler::emitOpF32ReinterpretI32() {
  return dispatchConversion1(ReinterpretI32AsF32, ValType::I32,
                                 ValType::F32);
}

bool BaseCompiler::emitOpF64ReinterpretI64() {
  return dispatchConversion1(ReinterpretI64AsF64, ValType::I64,
                                 ValType::F64);
}

bool BaseCompiler::emitOpUnreachable() {
  if (!deadCode_) {
    trap(Trap::Unreachable);
    deadCode_ = true;
  }
  return true;
}

bool BaseCompiler::emitOpNop() { return true; }

bool BaseCompiler::emitOpI32Const(int32_t value) {
  if (!deadCode_) {
    pushI32(value);
  }
  return true;
}

bool BaseCompiler::emitOpI64Const(int64_t value) {
  if (!deadCode_) {
    pushI64(value);
  }
  return true;
}

bool BaseCompiler::emitOpF32Const(float value) {
  if (!deadCode_) {
    pushF32(value);
  }
  return true;
}

bool BaseCompiler::emitOpF64Const(double value) {
  if (!deadCode_) {
    pushF64(value);
  }
  return true;
}

#undef dispatchBinary0
#undef dispatchBinary1
#undef dispatchBinary2
#undef dispatchBinary3
#undef dispatchUnary1
#undef dispatchUnary2
#undef dispatchComparison0
#undef dispatchConversion0
#undef dispatchConversion1
#undef dispatchConversionOOM
#undef dispatchCalloutConversionOOM
#undef dispatchIntDivCallout


//////////////////////////////////////////////////////////////////////////////
//
// Various helpers.

void BaseCompiler::assertResultRegistersAvailable(ResultType type) {
#ifdef DEBUG
  for (ABIResultIter iter(type); !iter.done(); iter.next()) {
    ABIResult result = iter.cur();
    if (!result.inRegister()) {
      return;
    }
    switch (result.type().kind()) {
      case ValType::I32:
        MOZ_ASSERT(isAvailableI32(RegI32(result.gpr())));
        break;
      case ValType::I64:
        MOZ_ASSERT(isAvailableI64(RegI64(result.gpr64())));
        break;
      case ValType::V128:
#  ifdef ENABLE_WASM_SIMD
        MOZ_ASSERT(isAvailableV128(RegV128(result.fpr())));
        break;
#  else
        MOZ_CRASH("No SIMD support");
#  endif
      case ValType::F32:
        MOZ_ASSERT(isAvailableF32(RegF32(result.fpr())));
        break;
      case ValType::F64:
        MOZ_ASSERT(isAvailableF64(RegF64(result.fpr())));
        break;
      case ValType::Ref:
        MOZ_ASSERT(isAvailableRef(RegRef(result.gpr())));
        break;
    }
  }
#endif
}

void BaseCompiler::saveTempPtr(const RegPtr& r) {
  MOZ_ASSERT(!ra.isAvailablePtr(r));
  fr.pushGPR(r);
  ra.freePtr(r);
  MOZ_ASSERT(ra.isAvailablePtr(r));
}

void BaseCompiler::restoreTempPtr(const RegPtr& r) {
  MOZ_ASSERT(ra.isAvailablePtr(r));
  ra.needPtr(r);
  fr.popGPR(r);
  MOZ_ASSERT(!ra.isAvailablePtr(r));
}

#ifdef DEBUG
void BaseCompiler::performRegisterLeakCheck() {
  BaseRegAlloc::LeakCheck check(ra);
  for (auto& item : stk_) {
    switch (item.kind_) {
      case Stk::RegisterI32:
        check.addKnownI32(item.i32reg());
        break;
      case Stk::RegisterI64:
        check.addKnownI64(item.i64reg());
        break;
      case Stk::RegisterF32:
        check.addKnownF32(item.f32reg());
        break;
      case Stk::RegisterF64:
        check.addKnownF64(item.f64reg());
        break;
#  ifdef ENABLE_WASM_SIMD
      case Stk::RegisterV128:
        check.addKnownV128(item.v128reg());
        break;
#  endif
      case Stk::RegisterRef:
        check.addKnownRef(item.refReg());
        break;
      default:
        break;
    }
  }
}

void BaseCompiler::assertStackInvariants() const {
  if (deadCode_) {
    // Nonlocal control flow can pass values in stack locations in a way that
    // isn't accounted for by the value stack.  In dead code, which occurs
    // after unconditional non-local control flow, there is no invariant to
    // assert.
    return;
  }
  size_t size = 0;
  for (const Stk& v : stk_) {
    switch (v.kind()) {
      case Stk::MemRef:
        size += BaseStackFrame::StackSizeOfPtr;
        break;
      case Stk::MemI32:
        size += BaseStackFrame::StackSizeOfPtr;
        break;
      case Stk::MemI64:
        size += BaseStackFrame::StackSizeOfInt64;
        break;
      case Stk::MemF64:
        size += BaseStackFrame::StackSizeOfDouble;
        break;
      case Stk::MemF32:
        size += BaseStackFrame::StackSizeOfFloat;
        break;
#  ifdef ENABLE_WASM_SIMD
      case Stk::MemV128:
        size += BaseStackFrame::StackSizeOfV128;
        break;
#  endif
      default:
        MOZ_ASSERT(!v.isMem());
        break;
    }
  }
  MOZ_ASSERT(size == fr.dynamicHeight());
}

void BaseCompiler::showStack(const char* who) const {
  fprintf(stderr, "Stack at %s {{\n", who);
  size_t n = 0;
  for (const Stk& elem : stk_) {
    fprintf(stderr, "  [%zu] ", n++);
    elem.showStackElem();
    fprintf(stderr, "\n");
  }
  fprintf(stderr, "}}\n");
}
#endif

//////////////////////////////////////////////////////////////////////////////
//
// Main compilation logic.

BaseCompiler::BaseCompiler(const CodeMetadata& codeMeta,
                           const CompilerEnvironment& compilerEnv,
                           const FuncCompileInput& func,
                           const RegisterOffsets& trapExitLayout,
                           size_t trapExitLayoutNumWords, Decoder& decoder,
                           StkVector& stkSource, TempAllocator* alloc,
                           MacroAssembler* masm, StackMaps* stackMaps)
    :  // Environment
      codeMeta_(codeMeta),
      compilerEnv_(compilerEnv),
      func_(func),
      previousBreakablePoint_(UINT32_MAX),
      stkSource_(stkSource),
      // Output-only data structures
      alloc_(alloc->fallible()),
      masm(*masm),
      // Compilation state
      decoder_(decoder),
      fr(*masm),
      stackMaps_(stackMaps),
      stackMapGenerator_(stackMaps, trapExitLayout, trapExitLayoutNumWords,
                         *masm),
      deadCode_(false),
      // Init value is selected to ensure proper logic in finishTryNote.
      mostRecentFinishedTryNoteIndex_(0),
      bceSafe_(0),
      latentOp_(LatentOp::None),
      latentType_(ValType::I32),
      latentIntCmp_(Assembler::Equal),
      latentDoubleCmp_(Assembler::DoubleEqual) {
  // Our caller, BaselineCompileFunctions, will lend us the vector contents to
  // use for the eval stack.  To get hold of those contents, we'll temporarily
  // installing an empty one in its place.
  MOZ_ASSERT(stk_.empty());
  stk_.swap(stkSource_);

  // Assuming that previously processed wasm functions are well formed, the
  // eval stack should now be empty.  But empty it anyway; any non-emptyness
  // at this point will cause chaos.
  stk_.clear();
}

BaseCompiler::~BaseCompiler() {
  stk_.swap(stkSource_);
  // We've returned the eval stack vector contents to our caller,
  // BaselineCompileFunctions.  We expect the vector we get in return to be
  // empty since that's what we swapped for the stack vector in our
  // constructor.
  MOZ_ASSERT(stk_.empty());
}

bool BaseCompiler::init(const ValTypeVector& locals) {
  // The compiler exists before the body starts, but its locals come from the
  // validator once the body's first hook fires.
  locals_ = &locals;

  // We may lift this restriction in the future.
  for (uint32_t memoryIndex = 0; memoryIndex < codeMeta_.memories.length();
       memoryIndex++) {
    MOZ_ASSERT_IF(isMem64(memoryIndex),
                  !codeMeta_.hugeMemoryEnabled(memoryIndex));
  }

  ra.init(this);

  if (!SigD_.append(ValType::F64)) {
    return false;
  }
  if (!SigF_.append(ValType::F32)) {
    return false;
  }

  ArgTypeVector args(funcType());
  return fr.setupLocals(locals, args, compilerEnv_.debugEnabled(),
                        &localInfo_);
}

FuncOffsets BaseCompiler::finish() {
  // The body is finished when its outermost block closed, which is what the
  // validator decided. Whether a decoder reached the end is only meaningful
  // for the dispatch that reads the body.
  MOZ_ASSERT(controlStack_.empty());
  MOZ_ASSERT(stk_.empty());
  MOZ_ASSERT(stackMapGenerator_.memRefsOnStk == 0);

  masm.flushBuffer();

  return offsets_;
}

}  // namespace wasm
}  // namespace js

// The rest of this file names CodeMetadata, BaseCompiler, Op, MaxLocals and
// friends unqualified, which only resolves under a unified build because
// WasmAnyRef.cpp, first in the unified blob, has a global `using namespace
// js::wasm`. Put the driver back in its own namespace so this file does not
// depend on that.
namespace js {
namespace wasm {

//////////////////////////////////////////////////////////////////////////////
//
// Driving the compiler from itasca.
//
// itasca owns the decode-and-validate loop and reports each operator it
// accepted, so the dispatch is a hook per operator that calls straight into
// the one method that emits it: no switch stands between the hook and the
// emitter.

namespace {

// Everything the hooks share for one function body.
//
// The compiler exists as soon as the driver does, but its locals come from
// the validator rather than from the bytes, so on_function_start reads them
// and calls compiler.init(locals) before the rest of the hooks run.
struct BaseDriver {
  // The function's locals, read by on_function_start out of the VwContext
  // before it calls compiler.init(locals).
  ValTypeVector locals;
  BaseCompiler compiler;

  // What to add to a reported operator offset to get a module offset. Zero
  // when the drive is over the whole module, and the code section's start when
  // it is over the section alone, which is all a later tier retains.
  uint32_t opOffsetBase = 0;

  // A br_table's labels arrive one hook at a time and are used by the hook
  // that closes the operator.
  Uint32Vector brTableDepths;

  // Why a hook declined. itasca reports that one did and nothing more.
  DeclineReason decline = DeclineReason::None;

  BaseDriver(const CodeMetadata& codeMeta,
             const CompilerEnvironment& compilerEnv,
             const FuncCompileInput& func,
             const RegisterOffsets& trapExitLayout,
             size_t trapExitLayoutNumWords, Decoder& decoder, StkVector& stk,
             TempAllocator* alloc, MacroAssembler* masm, StackMaps* stackMaps)
      : compiler(codeMeta, compilerEnv, func, trapExitLayout,
                trapExitLayoutNumWords, decoder, stk, alloc, masm,
                stackMaps) {}
};

BaseDriver* driver(const VwHookCtx* c) {
  return static_cast<BaseDriver*>(c->user);
}

}  // namespace

// The prologue every operator hook runs: sets up `f` and this operator's
// module offset, and bails out of the hook if it failed.
//
// The AutoCreatedBy scope covers everything the hook does with masm, since
// nothing between here and BC_OP_END has one of its own: every masm-emitting
// call this used to reach only through emitOneOp's own scope now runs here
// instead.
#define BC_OP_BEGIN(c, opcode)                                       \
  BaseDriver* st = driver(c);                                        \
  BaseCompiler& f = st->compiler;                                    \
  AutoCreatedBy acb(f.masm, "(wasm)SM_Baseline_on_*");                \
  size_t opOffset = st->opOffsetBase + (c)->op_offset;                \
  if (!f.beginPushedOp(OpBytes(opcode), opOffset)) {                 \
    st->decline = DeclineReason::OutOfMemory;                        \
    return VW_VISIT_OOM;                                            \
  }

// The epilogue: run the operator's own emitter and translate its result.
#define BC_OP_END(call)                                               \
  if (!(call)) {                                                      \
    st->decline = DeclineReason::OutOfMemory;                         \
    return VW_VISIT_OOM;                                             \
  }                                                                    \
  f.endPushedOp();                                                     \
  return VW_VISIT_OK;

// The hooks. Each names its own opcode and calls the one method that maps to
// it, so this is the whole of the dispatch: there is no second switch behind
// it.
//
// Each is a fixed-name extern "C" function rather than a VwVisitor entry:
// itasca_glue's BaselineVisitor (the Rust glue crate) calls these directly
// by name instead of through a vtable of function pointers, which is what
// lets cross-language LTO fold the call instead of leaving it indirect.
// Baseline is hot enough per operator for that indirection to show up in
// compile time; Ion's own optimizing backend dominates its own compile time,
// so WasmIonCompile.cpp's IonItascaVisitor keeps using the vtable.
extern "C" uint32_t SM_Baseline_on_function_start(const VwHookCtx* c,
                                                   const VwContext* ctx,
                                                   uint32_t, size_t, size_t) {
  BaseDriver* st = driver(c);

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
    st->locals.infallibleAppend(ToWasmValType(itasca_context_local(ctx, i)));
  }

  BaseCompiler& f = st->compiler;
  if (!f.init(st->locals) || !f.beginFunction() || !f.beginPushedBody()) {
    st->decline = DeclineReason::OutOfMemory;
    return VW_VISIT_OOM;
  }
  return VW_VISIT_OK;
}

extern "C" uint32_t SM_Baseline_on_block(const VwHookCtx* c, const VwState*,
                                         VwBlockType type) {
  BC_OP_BEGIN(c, Op::Block);
  BC_OP_END(f.emitBlock(ToWasmBlockType(type)));
}

extern "C" uint32_t SM_Baseline_on_loop(const VwHookCtx* c, const VwState*,
                                        VwBlockType type) {
  BC_OP_BEGIN(c, Op::Loop);
  BC_OP_END(f.emitLoop(ToWasmBlockType(type)));
}

extern "C" uint32_t SM_Baseline_on_if(const VwHookCtx* c, const VwState*,
                                      VwBlockType type) {
  BC_OP_BEGIN(c, Op::If);
  BC_OP_END(f.emitIf(ToWasmBlockType(type)));
}

extern "C" uint32_t SM_Baseline_on_else(const VwHookCtx* c, const VwState*,
                                        VwBlockType) {
  BC_OP_BEGIN(c, Op::Else);
  BC_OP_END(f.emitElse());
}

extern "C" uint32_t SM_Baseline_on_end(const VwHookCtx* c, const VwState*,
                                       VwLabelKind, VwBlockType) {
  BC_OP_BEGIN(c, Op::End);
  BC_OP_END(f.emitEnd());
}

extern "C" uint32_t SM_Baseline_on_br(const VwHookCtx* c, const VwState*,
                                      uint32_t depth, VwBlockType) {
  BC_OP_BEGIN(c, Op::Br);
  BC_OP_END(f.emitBr(depth));
}

extern "C" uint32_t SM_Baseline_on_br_if(const VwHookCtx* c, const VwState*,
                                         uint32_t depth, VwBlockType) {
  BC_OP_BEGIN(c, Op::BrIf);
  BC_OP_END(f.emitBrIf(depth));
}

// A br_table's labels arrive one at a time, and no hook says how many there
// will be, so they are collected until the operator closes.
extern "C" uint32_t SM_Baseline_on_br_table_label(const VwHookCtx* c,
                                                   const VwState*,
                                                   uint32_t depth) {
  BaseDriver* st = driver(c);
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

extern "C" uint32_t SM_Baseline_on_br_table(const VwHookCtx* c,
                                            const VwState*,
                                            uint32_t defaultDepth,
                                            VwBlockType) {
  BC_OP_BEGIN(c, Op::BrTable);
  if (!f.emitBrTable(st->brTableDepths, defaultDepth)) {
    st->decline = DeclineReason::OutOfMemory;
    return VW_VISIT_OOM;
  }
  st->brTableDepths.clear();
  f.endPushedOp();
  return VW_VISIT_OK;
}

extern "C" uint32_t SM_Baseline_on_drop(const VwHookCtx* c, const VwState*,
                                        VwStackType) {
  BC_OP_BEGIN(c, Op::Drop);
  BC_OP_END(f.emitDrop());
}

extern "C" uint32_t SM_Baseline_on_select(const VwHookCtx* c, const VwState*,
                                          VwStackType type) {
  BC_OP_BEGIN(c, Op::SelectNumeric);
  BC_OP_END(f.emitSelect(type));
}

extern "C" uint32_t SM_Baseline_on_i32_const(const VwHookCtx* c,
                                             const VwState*, int32_t value) {
  BC_OP_BEGIN(c, Op::I32Const);
  BC_OP_END(f.emitOpI32Const(value));
}

extern "C" uint32_t SM_Baseline_on_i64_const(const VwHookCtx* c,
                                             const VwState*, int64_t value) {
  BC_OP_BEGIN(c, Op::I64Const);
  BC_OP_END(f.emitOpI64Const(value));
}

extern "C" uint32_t SM_Baseline_on_f32_const(const VwHookCtx* c,
                                             const VwState*, uint32_t bits) {
  BC_OP_BEGIN(c, Op::F32Const);
  float value;
  memcpy(&value, &bits, sizeof(value));
  BC_OP_END(f.emitOpF32Const(value));
}

extern "C" uint32_t SM_Baseline_on_f64_const(const VwHookCtx* c,
                                             const VwState*, uint64_t bits) {
  BC_OP_BEGIN(c, Op::F64Const);
  double value;
  memcpy(&value, &bits, sizeof(value));
  BC_OP_END(f.emitOpF64Const(value));
}

extern "C" uint32_t SM_Baseline_on_unreachable(const VwHookCtx* c,
                                               const VwState*) {
  BC_OP_BEGIN(c, Op::Unreachable);
  BC_OP_END(f.emitOpUnreachable());
}

extern "C" uint32_t SM_Baseline_on_nop(const VwHookCtx* c, const VwState*) {
  BC_OP_BEGIN(c, Op::Nop);
  BC_OP_END(f.emitOpNop());
}

extern "C" uint32_t SM_Baseline_on_return(const VwHookCtx* c,
                                          const VwState*) {
  BC_OP_BEGIN(c, Op::Return);
  BC_OP_END(f.emitReturn());
}

extern "C" uint32_t SM_Baseline_on_memory_size(const VwHookCtx* c,
                                               const VwState*) {
  BC_OP_BEGIN(c, Op::MemorySize);
  BC_OP_END(f.emitMemorySize());
}

extern "C" uint32_t SM_Baseline_on_memory_grow(const VwHookCtx* c,
                                               const VwState*) {
  BC_OP_BEGIN(c, Op::MemoryGrow);
  BC_OP_END(f.emitMemoryGrow());
}

extern "C" uint32_t SM_Baseline_on_call(const VwHookCtx* c, const VwState*,
                                        uint32_t index) {
  BC_OP_BEGIN(c, Op::Call);
  BC_OP_END(f.emitCall(index));
}

extern "C" uint32_t SM_Baseline_on_call_indirect(const VwHookCtx* c,
                                                 const VwState*,
                                                 uint32_t index) {
  BC_OP_BEGIN(c, Op::CallIndirect);
  BC_OP_END(f.emitCallIndirect(index));
}

extern "C" uint32_t SM_Baseline_on_local_get(const VwHookCtx* c,
                                             const VwState*, uint32_t index) {
  BC_OP_BEGIN(c, Op::LocalGet);
  BC_OP_END(f.emitGetLocal(index));
}

extern "C" uint32_t SM_Baseline_on_local_set(const VwHookCtx* c,
                                             const VwState*, uint32_t index) {
  BC_OP_BEGIN(c, Op::LocalSet);
  BC_OP_END(f.emitSetLocal(index));
}

extern "C" uint32_t SM_Baseline_on_local_tee(const VwHookCtx* c,
                                             const VwState*, uint32_t index) {
  BC_OP_BEGIN(c, Op::LocalTee);
  BC_OP_END(f.emitTeeLocal(index));
}

extern "C" uint32_t SM_Baseline_on_global_get(const VwHookCtx* c,
                                              const VwState*,
                                              uint32_t index) {
  BC_OP_BEGIN(c, Op::GlobalGet);
  BC_OP_END(f.emitGetGlobal(index));
}

extern "C" uint32_t SM_Baseline_on_global_set(const VwHookCtx* c,
                                              const VwState*,
                                              uint32_t index) {
  BC_OP_BEGIN(c, Op::GlobalSet);
  BC_OP_END(f.emitSetGlobal(index));
}

// The memory hooks: each names the (ValType, Scalar::Type) its opcode loads
// or stores. Generated from the mnemonics in SM_VW_FOR_EACH_MEMORY_OP, so a
// hook cannot be wired to the wrong shape; written out rather than
// table-generated because each needs different arguments to emitLoad or
// emitStore, which the two-column (hook, opcode) table does not carry.
extern "C" uint32_t SM_Baseline_on_i32_load(const VwHookCtx* c,
                                            const VwState*, VwMemArg m) {
  BC_OP_BEGIN(c, Op::I32Load);
  BC_OP_END(f.emitLoad(ValType::I32, Scalar::Int32, m.align, m.offset));
}

extern "C" uint32_t SM_Baseline_on_i64_load(const VwHookCtx* c,
                                            const VwState*, VwMemArg m) {
  BC_OP_BEGIN(c, Op::I64Load);
  BC_OP_END(f.emitLoad(ValType::I64, Scalar::Int64, m.align, m.offset));
}

extern "C" uint32_t SM_Baseline_on_f32_load(const VwHookCtx* c,
                                            const VwState*, VwMemArg m) {
  BC_OP_BEGIN(c, Op::F32Load);
  BC_OP_END(f.emitLoad(ValType::F32, Scalar::Float32, m.align, m.offset));
}

extern "C" uint32_t SM_Baseline_on_f64_load(const VwHookCtx* c,
                                            const VwState*, VwMemArg m) {
  BC_OP_BEGIN(c, Op::F64Load);
  BC_OP_END(f.emitLoad(ValType::F64, Scalar::Float64, m.align, m.offset));
}

extern "C" uint32_t SM_Baseline_on_i32_load8_s(const VwHookCtx* c,
                                               const VwState*, VwMemArg m) {
  BC_OP_BEGIN(c, Op::I32Load8S);
  BC_OP_END(f.emitLoad(ValType::I32, Scalar::Int8, m.align, m.offset));
}

extern "C" uint32_t SM_Baseline_on_i32_load8_u(const VwHookCtx* c,
                                               const VwState*, VwMemArg m) {
  BC_OP_BEGIN(c, Op::I32Load8U);
  BC_OP_END(f.emitLoad(ValType::I32, Scalar::Uint8, m.align, m.offset));
}

extern "C" uint32_t SM_Baseline_on_i32_load16_s(const VwHookCtx* c,
                                                const VwState*, VwMemArg m) {
  BC_OP_BEGIN(c, Op::I32Load16S);
  BC_OP_END(f.emitLoad(ValType::I32, Scalar::Int16, m.align, m.offset));
}

extern "C" uint32_t SM_Baseline_on_i32_load16_u(const VwHookCtx* c,
                                                const VwState*, VwMemArg m) {
  BC_OP_BEGIN(c, Op::I32Load16U);
  BC_OP_END(f.emitLoad(ValType::I32, Scalar::Uint16, m.align, m.offset));
}

extern "C" uint32_t SM_Baseline_on_i64_load8_s(const VwHookCtx* c,
                                               const VwState*, VwMemArg m) {
  BC_OP_BEGIN(c, Op::I64Load8S);
  BC_OP_END(f.emitLoad(ValType::I64, Scalar::Int8, m.align, m.offset));
}

extern "C" uint32_t SM_Baseline_on_i64_load8_u(const VwHookCtx* c,
                                               const VwState*, VwMemArg m) {
  BC_OP_BEGIN(c, Op::I64Load8U);
  BC_OP_END(f.emitLoad(ValType::I64, Scalar::Uint8, m.align, m.offset));
}

extern "C" uint32_t SM_Baseline_on_i64_load16_s(const VwHookCtx* c,
                                                const VwState*, VwMemArg m) {
  BC_OP_BEGIN(c, Op::I64Load16S);
  BC_OP_END(f.emitLoad(ValType::I64, Scalar::Int16, m.align, m.offset));
}

extern "C" uint32_t SM_Baseline_on_i64_load16_u(const VwHookCtx* c,
                                                const VwState*, VwMemArg m) {
  BC_OP_BEGIN(c, Op::I64Load16U);
  BC_OP_END(f.emitLoad(ValType::I64, Scalar::Uint16, m.align, m.offset));
}

extern "C" uint32_t SM_Baseline_on_i64_load32_s(const VwHookCtx* c,
                                                const VwState*, VwMemArg m) {
  BC_OP_BEGIN(c, Op::I64Load32S);
  BC_OP_END(f.emitLoad(ValType::I64, Scalar::Int32, m.align, m.offset));
}

extern "C" uint32_t SM_Baseline_on_i64_load32_u(const VwHookCtx* c,
                                                const VwState*, VwMemArg m) {
  BC_OP_BEGIN(c, Op::I64Load32U);
  BC_OP_END(f.emitLoad(ValType::I64, Scalar::Uint32, m.align, m.offset));
}

extern "C" uint32_t SM_Baseline_on_i32_store(const VwHookCtx* c,
                                             const VwState*, VwMemArg m) {
  BC_OP_BEGIN(c, Op::I32Store);
  BC_OP_END(f.emitStore(ValType::I32, Scalar::Int32, m.align, m.offset));
}

extern "C" uint32_t SM_Baseline_on_i64_store(const VwHookCtx* c,
                                             const VwState*, VwMemArg m) {
  BC_OP_BEGIN(c, Op::I64Store);
  BC_OP_END(f.emitStore(ValType::I64, Scalar::Int64, m.align, m.offset));
}

extern "C" uint32_t SM_Baseline_on_f32_store(const VwHookCtx* c,
                                             const VwState*, VwMemArg m) {
  BC_OP_BEGIN(c, Op::F32Store);
  BC_OP_END(f.emitStore(ValType::F32, Scalar::Float32, m.align, m.offset));
}

extern "C" uint32_t SM_Baseline_on_f64_store(const VwHookCtx* c,
                                             const VwState*, VwMemArg m) {
  BC_OP_BEGIN(c, Op::F64Store);
  BC_OP_END(f.emitStore(ValType::F64, Scalar::Float64, m.align, m.offset));
}

extern "C" uint32_t SM_Baseline_on_i32_store8(const VwHookCtx* c,
                                              const VwState*, VwMemArg m) {
  BC_OP_BEGIN(c, Op::I32Store8);
  BC_OP_END(f.emitStore(ValType::I32, Scalar::Int8, m.align, m.offset));
}

extern "C" uint32_t SM_Baseline_on_i32_store16(const VwHookCtx* c,
                                               const VwState*, VwMemArg m) {
  BC_OP_BEGIN(c, Op::I32Store16);
  BC_OP_END(f.emitStore(ValType::I32, Scalar::Int16, m.align, m.offset));
}

extern "C" uint32_t SM_Baseline_on_i64_store8(const VwHookCtx* c,
                                              const VwState*, VwMemArg m) {
  BC_OP_BEGIN(c, Op::I64Store8);
  BC_OP_END(f.emitStore(ValType::I64, Scalar::Int8, m.align, m.offset));
}

extern "C" uint32_t SM_Baseline_on_i64_store16(const VwHookCtx* c,
                                               const VwState*, VwMemArg m) {
  BC_OP_BEGIN(c, Op::I64Store16);
  BC_OP_END(f.emitStore(ValType::I64, Scalar::Int16, m.align, m.offset));
}

extern "C" uint32_t SM_Baseline_on_i64_store32(const VwHookCtx* c,
                                               const VwState*, VwMemArg m) {
  BC_OP_BEGIN(c, Op::I64Store32);
  BC_OP_END(f.emitStore(ValType::I64, Scalar::Int32, m.align, m.offset));
}

// The numeric hooks: no immediates, one emitter per opcode, generated from
// SM_VW_FOR_EACH_NUMERIC_OP so a hook cannot be mapped to the wrong
// operator.
#define BC_DECLARE_NUMERIC_HOOK(hook, Name)                                 \
  extern "C" uint32_t SM_Baseline_##hook(const VwHookCtx* c,                \
                                         const VwState*) {                   \
    BC_OP_BEGIN(c, Op::Name);                                               \
    BC_OP_END(f.emitOp##Name());                                            \
  }
SM_VW_FOR_EACH_NUMERIC_OP(BC_DECLARE_NUMERIC_HOOK)
#undef BC_DECLARE_NUMERIC_HOOK

// One pointer per hook, listed once so a hook whose extern "C" definition
// above went missing or was misnamed is a build error here rather than a
// silent gap: baseline would otherwise emit no code for that operator and
// nothing here would say why. Mirrors the completeness check
// WasmItascaOps.h's static_asserts give the vtable Ion still uses.
namespace {
#define BC_HOOK_PTR(hook, Name) reinterpret_cast<void*>(&SM_Baseline_##hook),
[[maybe_unused]] const void* const kBaselineHooks[] = {
    reinterpret_cast<void*>(&SM_Baseline_on_function_start),
    reinterpret_cast<void*>(&SM_Baseline_on_unreachable),
    reinterpret_cast<void*>(&SM_Baseline_on_nop),
    reinterpret_cast<void*>(&SM_Baseline_on_block),
    reinterpret_cast<void*>(&SM_Baseline_on_loop),
    reinterpret_cast<void*>(&SM_Baseline_on_if),
    reinterpret_cast<void*>(&SM_Baseline_on_else),
    reinterpret_cast<void*>(&SM_Baseline_on_end),
    reinterpret_cast<void*>(&SM_Baseline_on_br),
    reinterpret_cast<void*>(&SM_Baseline_on_br_if),
    reinterpret_cast<void*>(&SM_Baseline_on_br_table_label),
    reinterpret_cast<void*>(&SM_Baseline_on_br_table),
    reinterpret_cast<void*>(&SM_Baseline_on_return),
    reinterpret_cast<void*>(&SM_Baseline_on_call),
    reinterpret_cast<void*>(&SM_Baseline_on_call_indirect),
    reinterpret_cast<void*>(&SM_Baseline_on_drop),
    reinterpret_cast<void*>(&SM_Baseline_on_select),
    reinterpret_cast<void*>(&SM_Baseline_on_local_get),
    reinterpret_cast<void*>(&SM_Baseline_on_local_set),
    reinterpret_cast<void*>(&SM_Baseline_on_local_tee),
    reinterpret_cast<void*>(&SM_Baseline_on_global_get),
    reinterpret_cast<void*>(&SM_Baseline_on_global_set),
    reinterpret_cast<void*>(&SM_Baseline_on_memory_size),
    reinterpret_cast<void*>(&SM_Baseline_on_memory_grow),
    reinterpret_cast<void*>(&SM_Baseline_on_i32_const),
    reinterpret_cast<void*>(&SM_Baseline_on_i64_const),
    reinterpret_cast<void*>(&SM_Baseline_on_f32_const),
    reinterpret_cast<void*>(&SM_Baseline_on_f64_const),
    SM_VW_FOR_EACH_MEMORY_OP(BC_HOOK_PTR)
    SM_VW_FOR_EACH_NUMERIC_OP(BC_HOOK_PTR)
};
#undef BC_HOOK_PTR
static_assert(sizeof(kBaselineHooks) / sizeof(kBaselineHooks[0]) == 174,
              "SM_Baseline_on_* has drifted from VwVisitor's 174 hooks");
}  // namespace

#undef BC_OP_BEGIN
#undef BC_OP_END

}  // namespace wasm
}  // namespace js

bool js::wasm::BaselinePlatformSupport() {
#if defined(JS_CODEGEN_ARM)
  // Simplifying assumption: require SDIV and UDIV.
  //
  // I have no good data on ARM populations allowing me to say that
  // X% of devices in the market implement SDIV and UDIV.  However,
  // they are definitely implemented on the Cortex-A7 and Cortex-A15
  // and on all ARMv8 systems.
  if (!ARMFlags::HasIDIV()) {
    return false;
  }
#endif
#if defined(JS_CODEGEN_X64) || defined(JS_CODEGEN_X86) ||        \
    defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_ARM64) ||      \
    defined(JS_CODEGEN_MIPS64) || defined(JS_CODEGEN_LOONG64) || \
    defined(JS_CODEGEN_RISCV64)
  return true;
#else
  return false;
#endif
}

bool js::wasm::BaselineCompileFunctions(
    const CodeMetadata& codeMeta, const CompilerEnvironment& compilerEnv,
    LifoAlloc& lifo, const FuncCompileInputVector& inputs, CompiledCode* code,
    const BytecodeRegion& codeBytes, const VwEnv* itascaEnv, UniqueChars* error,
    VwBatch* validationBatch) {
  MOZ_ASSERT(compilerEnv.tier() == Tier::Baseline);

  // The MacroAssembler will sometimes access the jitContext.

  TempAllocator alloc(&lifo);
  JitContext jitContext;
  MOZ_ASSERT(IsCompilingWasm());
  WasmMacroAssembler masm(alloc);

  // Swap in already-allocated empty vectors to avoid malloc/free.
  MOZ_ASSERT(code->empty());
  if (!code->swap(masm)) {
    return false;
  }

  // Create a description of the stack layout created by GenerateTrapExit().
  RegisterOffsets trapExitLayout;
  size_t trapExitLayoutNumWords;
  GenerateTrapExitRegisterOffsets(&trapExitLayout, &trapExitLayoutNumWords);

  // The compiler's operand stack.  We reuse it across all functions so as to
  // avoid malloc/free.  Presize it to 128 elements in the hope of avoiding
  // reallocation later.
  StkVector stk;
  if (!stk.reserve(128)) {
    return false;
  }

  for (const FuncCompileInput& func : inputs) {
    JitSpew(JitSpew_Codegen,
            "# ================================"
            "================================");
    JitSpew(JitSpew_Codegen,
            "# j::w::BaselineCompileFunctions: BEGIN function index %d",
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

    // Nothing reads bytes through this any more: the validator does the
    // reading. It is still here for the odd error report that names a byte
    // offset the validator did not.
    Decoder d(func.begin, func.end, func.bytecodeOffset, error);

    size_t unwindInfoBefore = masm.codeRangeUnwindInfos().length();
    size_t callRefMetricsBefore = masm.callRefMetricsPatches().length();
    size_t allocSitesBefore = masm.allocSitesPatches().length();

    // One-pass baseline compilation, driven by itasca: the compiler exists
    // now, but its locals come from the validator, so on_function_start
    // finishes initializing it once its own hook fires.
    BaseDriver driver(codeMeta, compilerEnv, func, trapExitLayout,
                      trapExitLayoutNumWords, d, stk, &alloc, &masm,
                      &code->stackMaps);
    driver.opOffsetBase =
        codeBytes.start + (validationBatch ? func.entryOffset : 0);

    // Every body reaching baseline came out of a module itasca validated.
    // The engine's own synthetic modules, which built their bodies rather than
    // parsing them, are excluded from this prototype.
    MOZ_RELEASE_ASSERT(itascaEnv);
    if (!DriveBodyWithItascaBaseline(codeBytes, func.entryOffset, itascaEnv,
                                     func.index - codeMeta.numFuncImports,
                                     &driver, &driver.decline, error,
                                     validationBatch)) {
      return false;
    }

    BaseCompiler& f = driver.compiler;
    if (!f.endFunction()) {
      return false;
    }
    FuncOffsets offsets(f.finish());
    bool hasUnwindInfo =
        unwindInfoBefore != masm.codeRangeUnwindInfos().length();
    size_t callRefMetricsAfter = masm.callRefMetricsPatches().length();
    size_t callRefMetricsLength = callRefMetricsAfter - callRefMetricsBefore;
    size_t allocSitesAfter = masm.allocSitesPatches().length();
    size_t allocSitesLength = allocSitesAfter - allocSitesBefore;

    // Record this function's code range
    if (!code->codeRanges.emplaceBack(func.index, offsets, hasUnwindInfo)) {
      return false;
    }

    // Record this function's specific feature usage: always none, since only
    // wasm 1.0 opcodes are reachable from the hooks.
    if (!code->funcs.emplaceBack(
            func.index, FeatureUsage::None,
            CallRefMetricsRange(callRefMetricsBefore, callRefMetricsLength),
            AllocSitesRange(allocSitesBefore, allocSitesLength))) {
      return false;
    }

    if (PerfEnabled()) {
      if (!code->funcBaselineSpewers.emplaceBack(func.index,
                                                 std::move(f.perfSpewer_))) {
        return false;
      }
    }

    // Accumulate observed feature usage: always none, as above.
    code->featureUsage |= FeatureUsage::None;

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
            "# j::w::BaselineCompileFunctions: END function index %d",
            (int)func.index);
    JitSpew(JitSpew_Codegen,
            "# ================================"
            "================================");
  }

  masm.finish();
  if (masm.oom()) {
    return false;
  }

  return code->swap(masm);
}
