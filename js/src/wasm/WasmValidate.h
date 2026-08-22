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

#ifndef wasm_validate_h
#define wasm_validate_h

#include "js/Utility.h"
#include "js/WasmFeatures.h"
#include "wasm/WasmBinary.h"
#include "wasm/WasmCompile.h"
#include "wasm/WasmCompileArgs.h"
#include "wasm/WasmModuleTypes.h"
#include "wasm/WasmOpIter.h"
#include "wasm/WasmProcess.h"
#include "wasm/WasmTypeDef.h"

namespace js {
namespace wasm {

// OpIter specialized for reading a module without compiling it, which is what
// the dumper and the initialiser-expression reader do. Validation itself is
// itasca's.

class NothingVector {
  mozilla::Nothing unused_;

 public:
  bool reserve(size_t size) { return true; }
  bool resize(size_t length) { return true; }
  mozilla::Nothing& operator[](size_t) { return unused_; }
  mozilla::Nothing& back() { return unused_; }
  size_t length() const { return 0; }
  bool append(mozilla::Nothing& nothing) { return true; }
  void infallibleAppend(mozilla::Nothing& nothing) {}
};

struct ValidatingPolicy {
  using Value = mozilla::Nothing;
  using ValueVector = NothingVector;
  using ControlItem = mozilla::Nothing;
};

template <typename Policy>
class OpIter;

using ValidatingOpIter = OpIter<ValidatingPolicy>;

// Shared subtyping function across validation.

[[nodiscard]] bool CheckIsSubtypeOf(Decoder& d, const CodeMetadata& codeMeta,
                                    size_t opcodeOffset, ResultType subType,
                                    ResultType superType);
[[nodiscard]] bool CheckIsSubtypeOf(Decoder& d, const CodeMetadata& codeMeta,
                                    size_t opcodeOffset, StorageType subType,
                                    StorageType superType);

// The local entries are part of function bodies and thus serialized and
// decoded as part of both validation and compilation.

[[nodiscard]] bool EncodeLocalEntries(Encoder& e, const ValTypeVector& locals);

// This performs no validation; the local entries must already have been
// validated by an earlier pass.

[[nodiscard]] bool DecodeValidatedLocalEntries(const TypeContext& types,
                                               Decoder& d,
                                               ValTypeVector* locals);

// This validates the entries. Function params are inserted before the locals
// to generate the full local entries for use in validation

[[nodiscard]] bool DecodeLocalEntriesWithParams(Decoder& d,
                                                const CodeMetadata& codeMeta,
                                                uint32_t funcIndex,
                                                ValTypeVector* locals);

// Returns whether the given [begin, end) prefix of a module's bytecode starts a
// code section and, if so, returns the BytecodeRange of that code section.
// Note that, even if this function returns 'false', [begin, end) may actually
// be a valid module in the special case when there are no function defs and the
// code section is not present. Such modules can be valid so the caller must
// handle this special case.
//
// The range runs from the section's id byte rather than from its contents, so
// that a bytecode source split on it hands itasca the section's own framing
// to read. See BytecodeSource.

[[nodiscard]] bool StartsCodeSection(const uint8_t* begin, const uint8_t* end,
                                     BytecodeRange* codeSection);

#ifdef ENABLE_WASM_COMPONENTS
[[nodiscard]] bool IsComponent(Decoder& d);
#endif

// Check the decoded imports against any enabled builtin module. Part of
// decoding the environment, and separate only because the environment can also
// come from itasca. `d` is used for error reporting and nothing else.

[[nodiscard]] bool CheckBuiltinModuleImports(Decoder& d,
                                             CodeMetadata* codeMeta,
                                             ModuleMetadata* moduleMeta);

// Decode the name section's subsections. `customSectionIndex` selects the
// section from CodeMetadata::customSectionRanges, which must already hold it.
// Cannot fail: a malformed name section is dropped, since nothing about a
// module's validity depends on its contents.

void DecodeNameSectionPayload(const BytecodeSpan& fromPayload,
                              uint32_t customSectionIndex,
                              CodeMetadata* codeMeta,
                              ModuleMetadata* moduleMeta,
                              UniqueCharsVector* warnings);

#ifdef ENABLE_WASM_COMPONENTS
[[nodiscard]] bool DecodeComponent(
    Decoder& d, MutableComponent c, const CompileArgs& args,
    JS::OptimizedEncodingListener* listener = nullptr);
#endif

// Validate an entire module, returning true if the module was validated
// successfully. If Validate returns false:
//  - if *error is null, the caller should report out-of-memory
//  - otherwise, there was a legitimate error described by *error

[[nodiscard]] bool Validate(JSContext* cx, const BytecodeSource& bytecode,
                            const FeatureOptions& options, UniqueChars* error);

struct NopOpDumper {
  void dumpOpBegin(OpBytes op) {}
  void dumpOpEnd() {}
  void dumpTypeIndex(uint32_t typeIndex, bool asTypeUse = false) {}
  void dumpFuncIndex(uint32_t funcIndex) {}
  void dumpTableIndex(uint32_t tableIndex) {}
  void dumpGlobalIndex(uint32_t globalIndex) {}
  void dumpMemoryIndex(uint32_t memoryIndex) {}
  void dumpElemIndex(uint32_t elemIndex) {}
  void dumpDataIndex(uint32_t dataIndex) {}
  void dumpTagIndex(uint32_t tagIndex) {}
  void dumpLocalIndex(uint32_t localIndex) {}
  void dumpBlockType(BlockType type) {}
  void dumpI32Const(int32_t constant) {}
  void dumpI64Const(int64_t constant) {}
  void dumpF32Const(float constant) {}
  void dumpF64Const(double constant) {}
  void dumpV128Const(V128 constant) {}
  void dumpVectorMask(V128 mask) {}
  void dumpRefType(RefType type) {}
  void dumpHeapType(RefType type) {}
  void dumpValType(ValType type) {}
  void dumpTryTableCatches(const TryTableCatchVector& catches) {}
  void dumpLinearMemoryAddress(LinearMemoryAddress<mozilla::Nothing> addr) {}
  void dumpBlockDepth(uint32_t relativeDepth) {}
  void dumpBlockDepths(const Uint32Vector& relativeDepths) {}
  void dumpFieldIndex(uint32_t fieldIndex) {}
  void dumpNumElements(uint32_t numElements) {}
  void dumpLaneIndex(uint32_t laneIndex) {}

  void startScope() {};
  void endScope() {};
};

template <class T>
[[nodiscard]] bool ValidateOps(ValidatingOpIter& iter, T& dumper,
                               const CodeMetadata& codeMeta);

}  // namespace wasm
}  // namespace js

#endif  // namespace wasm_validate_h
