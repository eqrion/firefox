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

#ifndef wasm_itasca_module_h
#define wasm_itasca_module_h

#include "mozilla/RefPtr.h"
#include "mozilla/UniquePtr.h"

#include "wasm/WasmBinaryTypes.h"
#include "wasm/WasmShareable.h"
#include "wasm/WasmTypeDecls.h"
#include "wasm/itasca.h"

namespace js {
namespace wasm {

struct VwEnvDeleter {
  void operator()(VwEnv* p) { itasca_env_free(p); }
};

struct VwBatchDeleter {
  void operator()(VwBatch* p) { itasca_batch_free(p); }
};
using UniqueVwBatch = mozilla::UniquePtr<VwBatch, VwBatchDeleter>;

using UniqueVwEnv = mozilla::UniquePtr<VwEnv, VwEnvDeleter>;

// A validated environment that outlives the compilation that produced it.
//
// Tier-2 recompiles a function body, which means driving itasca over it
// again, which needs the environment the body was validated against. Nothing
// else about a module's bytes survives its first compilation, so this holds
// what a later compilation needs: the environment, and the code section, which
// is where the bodies are.
//
// Only the code section, not the whole module: validating one entry reads
// nothing outside that entry, so an entry can be pointed at within the section
// as long as its position is taken relative to the section too.
struct ItascaModule : public AtomicRefCounted<ItascaModule> {
  UniqueVwEnv env;
  SharedBytes codeSection;
  // Where the code section starts in the module. An entry's position within
  // the section is its module position less this.
  uint32_t codeSectionStart = 0;

  BytecodeRegion codeRegion() const {
    BytecodeSpan span =
        codeSection ? BytecodeSpan(codeSection->begin(), codeSection->length())
                    : BytecodeSpan();
    return BytecodeRegion(span, codeSectionStart);
  }
};

using SharedItascaModule = RefPtr<ItascaModule>;

}  // namespace wasm
}  // namespace js

#endif  // wasm_itasca_module_h
