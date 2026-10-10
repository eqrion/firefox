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

#ifndef wasm_itasca_glue_h
#define wasm_itasca_glue_h

#include "mozilla/Maybe.h"
#include "mozilla/UniquePtr.h"

#include "js/Utility.h"
#include "wasm/WasmBinaryTypes.h"
#include "wasm/WasmCompileArgs.h"
#include "wasm/WasmConstants.h"
#include "wasm/WasmMetadata.h"
#include "wasm/WasmItascaModule.h"
#include "wasm/itasca.h"

namespace js {
namespace wasm {

class ModuleGenerator;

// Owned handles from itasca's C ABI. Every free accepts null.
struct VwTailDeleter {
  void operator()(VwTail* p) { itasca_tail_free(p); }
};

using UniqueVwTail = mozilla::UniquePtr<VwTail, VwTailDeleter>;

// Why one of our hooks declined a function body.
//
// itasca reports every decline as VW_OP_ERR_VISITOR_OOM, since from its side
// a consumer that refuses is a consumer that refuses. The reason therefore has
// to travel in the hook's own state rather than in the status.
enum class DeclineReason {
  None,
  OutOfMemory,
  TooManyLocals,
  BrTableTooBig,
};

// The state handed to every hook through VwHookCtx::user by the validate-only
// path. It records nothing about the body: the caps below are all itasca has
// no analogue for, and enforcing them is the only reason this path installs
// hooks at all.
struct LimitsVisitorState {
  DeclineReason decline = DeclineReason::None;
  uint32_t brTableElems = 0;
};

// Fill in the hooks that enforce the two per-body implementation caps:
// MaxLocals counted including parameters, and MaxBrTableElems.
void InitLimitsVisitor(VwVisitor* visitor);

// Every slot in a compiler's visitor is filled.
//
// itasca skips a null hook by design, which is what lets a validate-only
// consumer install only the hooks it wants. For a compiler that is the wrong
// default: a hook left out is an operator the compiler is never told about, so
// the body compiles to code that is missing it, silently. A release assert
// because nothing else catches it, and it runs once per process.
void AssertVisitorIsComplete(const VwVisitor& visitor);

// Enforce the caps in WasmConstants.h that itasca's src/limits.rs
// deliberately does not carry, so that a module SpiderMonkey cannot represent
// is rejected rather than crashing it. See the note in itasca's
// DEVELOPMENT.md on why they stay on this side of the boundary.
[[nodiscard]] bool CheckEnvLimits(const VwEnv* env, UniqueChars* error);
[[nodiscard]] bool CheckTailLimits(const VwTail* tail, UniqueChars* error);

// Report a rejection as the message SpiderMonkey shows, and return false.
//
// Two entry points because only one of the two has a position to report.
// itasca's module-level errors carry no byte offset, by a deliberate choice
// documented in its DEVELOPMENT.md, so printing one would be inventing it. A
// body is different: the offset of the operator a rejected body stopped at is
// what itasca_validate_code_entry hands back.
//
// A decline whose reason is OutOfMemory leaves *error null, which is how
// callers of Validate tell OOM from a real rejection.
[[nodiscard]] bool ReportItascaError(VwStatus status, UniqueChars* error);
[[nodiscard]] bool ReportItascaBodyError(VwStatus status, size_t offset,
                                           DeclineReason decline,
                                           UniqueChars* error);

// Report a rejection this side of the boundary noticed, and return false.
[[nodiscard]] bool FailItasca(UniqueChars* error, const char* msg);

// How much of the code section has arrived, for a caller that is streaming it.
//
// Reading a byte that has not arrived reads uninitialized memory, so the walk
// waits for the bytes of each read before making it. The default is a region
// that arrived whole, which waits for nothing.
class CodeSectionArrival {
 public:
  virtual ~CodeSectionArrival() = default;

  // Wait until the region's first `end` bytes have arrived, where `end` past
  // the region's end means all of it. False if the wait was cancelled, which
  // abandons the compilation.
  [[nodiscard]] virtual bool waitFor(size_t end) { return true; }
};

// Decode and validate a whole module. Returns false with *error set on a
// rejection, or false with *error null on out-of-memory.
[[nodiscard]] bool ValidateWithItasca(const BytecodeSource& bytecode,
                                        UniqueChars* error);

// Custom-section metadata collected across the environment and tail.
struct CustomSections {
  mozilla::Maybe<uint32_t> nameSectionIndex;
  mozilla::Maybe<BytecodeSpan> nameSectionBytes;
};

[[nodiscard]] bool BuildEnvWithItasca(const VwEnv*, ModuleMetadata*,
                                      UniqueChars*);
[[nodiscard]] bool BuildTailWithItasca(const VwTail*, ModuleMetadata*,
                                       UniqueChars*);
uint32_t RecordCustomSectionWithItasca(const BytecodeRegion&, CodeMetadata*,
                                       CustomSections*, size_t, size_t, size_t);

// A value type a hook reports, in SpiderMonkey's terms. The enum values are
// the binary format's, which are TypeCode's, so this is a check and not a
// conversion.
ValType ToWasmValType(VwValType type);

// A block type a hook reports, in SpiderMonkey's terms. wasm 1.0 blocks take
// no parameters and return at most one value, so the whole of it fits in the
// one byte itasca hands over.
BlockType ToWasmBlockType(VwBlockType type);

// A stack type a hook reports, in SpiderMonkey's terms. Crashes on
// VwStackType::Bot: a caller that can be handed one, because the operand is
// unconstrained in unreachable code, must check for dead code first.
ValType ToWasmValType(VwStackType type);

// Drive one function body through `visitor`, whose hooks receive `user`.
//
// This is validation and compilation in one pass: itasca owns the
// decode-and-validate loop and the hooks are the compiler. `*decline` is where
// a hook that refused the body left its reason, since itasca reports only
// that one did.
[[nodiscard]] bool DriveBodyWithItasca(
    const BytecodeRegion& code, size_t entryOffset, const VwEnv* env,
    uint32_t funcDefIndex, const VwVisitor& visitor, void* user,
    const DeclineReason* decline, UniqueChars* error, VwBatch* batch = nullptr);

// The same drive, for baseline's hooks specifically. Baseline's hooks are
// fixed extern "C" symbols rather than entries in a VwVisitor, so
// itasca_glue's baseline visitor calls them directly instead of through a
// vtable, which is what makes them inlinable across the FFI boundary; there
// is accordingly no `visitor` argument to take.
[[nodiscard]] bool DriveBodyWithItascaBaseline(
    const BytecodeRegion& code, size_t entryOffset, const VwEnv* env,
    uint32_t funcDefIndex, void* user, const DeclineReason* decline,
    UniqueChars* error, VwBatch* batch = nullptr);

void DecodeNameSectionWithItasca(const CustomSections& customs,
                                   ModuleMetadata* moduleMeta,
                                   UniqueCharsVector* warnings);

}  // namespace wasm
}  // namespace js

#endif  // wasm_itasca_glue_h
