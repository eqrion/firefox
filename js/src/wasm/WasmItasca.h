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

// What DecodeEnvWithItasca and DecodeTailWithItasca accumulate as they
// report each custom section they pass over, for DecodeNameSectionWithItasca
// to decode afterwards.
//
// A name section may be on either side of the code section, so this has to
// survive both calls: `nameSectionIndex` is where it landed in
// CodeMetadata::customSectionRanges, and `nameSectionBytes` is its payload,
// which is a span into whichever region reported it and stays valid once that
// region does.
struct CustomSections {
  mozilla::Maybe<uint32_t> nameSectionIndex;
  mozilla::Maybe<BytecodeSpan> nameSectionBytes;
};

// Decode and validate everything before the code section, and translate it into
// the metadata SpiderMonkey compiles against. `env` is the module's first
// region, which begins with its header. `*outEnv` is the environment the
// per-function validation then runs against, and `*outCodePos` is where the
// environment ended, which is the code section's header when there is one.
[[nodiscard]] bool DecodeEnvWithItasca(const BytecodeRegion& env,
                                         ModuleMetadata* moduleMeta,
                                         CustomSections* customs,
                                         UniqueVwEnv* outEnv,
                                         size_t* outCodePos,
                                         UniqueChars* error);

// Where the code section and the tail are, given where the environment ended.
//
// The two are the same region for a module with no code section: what follows
// the environment is the tail, in the environment's own region. Fails if the
// split and the environment do not end in the same place, which is a section
// before the code section that belongs in neither.
[[nodiscard]] bool RegionsAfterEnv(const BytecodeSource& bytecode,
                                   size_t codePos, BytecodeRegion* code,
                                   BytecodeRegion* tail, UniqueChars* error);

// Walk the code section, handing each entry to `mg.compileFuncDef`.
//
// `code` is the section, from its id byte to its last content byte, which is
// the region a bytecode source is split on. itasca_validate_code_with frames
// each entry from its own size prefix and hands it over unvalidated, so entries
// can be dispatched to compile threads before any of them is checked, and it
// shows the entries tile the section exactly before returning: a partition it
// hands out is the one itasca itself would produce walking the section
// itself.
[[nodiscard]] bool DecodeCodeSectionWithItasca(const BytecodeRegion& code,
                                                 const VwEnv* env,
                                                 const CodeMetadata& codeMeta,
                                                 ModuleGenerator& mg,
                                                 CodeSectionArrival& arrival,
                                                 UniqueChars* error);

// Check that itasca agrees that a module the splitter found no code section
// in has none: the function section may have declared bodies for one, and a
// section too big for the splitter to take is one it did not find.
//
// `region` is the module's bytes from where the environment ended.
[[nodiscard]] bool CheckNoCodeSectionWithItasca(const BytecodeRegion& region,
                                                  const VwEnv* env,
                                                  UniqueChars* error);

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
[[nodiscard]] bool DriveBodyWithItasca(const BytecodeRegion& code,
                                         size_t entryOffset, const VwEnv* env,
                                         uint32_t funcDefIndex,
                                         const VwVisitor& visitor, void* user,
                                         const DeclineReason* decline,
                                         UniqueChars* error);

// The same drive, for baseline's hooks specifically. Baseline's hooks are
// fixed extern "C" symbols rather than entries in a VwVisitor, so
// itasca_glue's baseline visitor calls them directly instead of through a
// vtable, which is what makes them inlinable across the FFI boundary; there
// is accordingly no `visitor` argument to take.
[[nodiscard]] bool DriveBodyWithItascaBaseline(const BytecodeRegion& code,
                                                 size_t entryOffset,
                                                 const VwEnv* env,
                                                 uint32_t funcDefIndex,
                                                 void* user,
                                                 const DeclineReason* decline,
                                                 UniqueChars* error);

// Decode and validate everything after the code section, and translate it.
// `tail` is the module's bytes from where the code section ended.
//
// This fills ModuleMetadata::dataSegments directly rather than the ranges the
// old decoder produced: a validator that reports a segment's bytes has no byte
// range to hand over, and it already copied them.
[[nodiscard]] bool DecodeTailWithItasca(const BytecodeRegion& tail,
                                          const VwEnv* env,
                                          ModuleMetadata* moduleMeta,
                                          CustomSections* customs,
                                          UniqueChars* error);

// Decode the name section DecodeEnvWithItasca and DecodeTailWithItasca
// found, if either found one. A custom section is allowed between any two
// others and the code section cannot hold one, so the two calls between them
// see every custom section in module order regardless of which side of the
// code section it is on, and `customs` is where they left what they found.
void DecodeNameSectionWithItasca(const CustomSections& customs,
                                   ModuleMetadata* moduleMeta,
                                   UniqueCharsVector* warnings);

}  // namespace wasm
}  // namespace js

#endif  // wasm_itasca_glue_h
