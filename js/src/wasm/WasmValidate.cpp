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

#include "wasm/WasmValidate.h"

#include "mozilla/CheckedInt.h"
#include "mozilla/ScopeExit.h"
#include "mozilla/Span.h"
#include "mozilla/Utf8.h"

#include "js/Printf.h"
#include "js/String.h"  // JS::MaxStringLength
#include "vm/JSContext.h"
#include "vm/Realm.h"
#include "wasm/WasmCompile.h"
#include "wasm/WasmConstants.h"
#include "wasm/WasmDump.h"
#include "wasm/WasmInitExpr.h"
#include "wasm/WasmOpIter.h"
#include "wasm/WasmTypeDecls.h"
#include "wasm/WasmItasca.h"

using namespace js;
using namespace js::jit;
using namespace js::wasm;

using mozilla::AsChars;
using mozilla::CheckedInt;
using mozilla::IsUtf8;
using mozilla::Maybe;
using mozilla::Nothing;
using mozilla::Some;
using mozilla::Span;

// Misc helpers.

// The debugger reports a frame's local types, which means reading the
// declarations that describe it. Reading, not validating: the module was
// validated when it was compiled.
bool wasm::DecodeValidatedLocalEntries(const TypeContext& types, Decoder& d,
                                       ValTypeVector* locals) {
  uint32_t numLocalEntries;
  MOZ_ALWAYS_TRUE(d.readVarU32(&numLocalEntries));

  for (uint32_t i = 0; i < numLocalEntries; i++) {
    uint32_t count = d.uncheckedReadVarU32();
    MOZ_ASSERT(MaxLocals - locals->length() >= count);
    if (!locals->appendN(d.uncheckedReadValType(types), count)) {
      return false;
    }
  }

  return true;
}

bool wasm::EncodeLocalEntries(Encoder& e, const ValTypeVector& locals) {
  if (locals.length() > MaxLocals) {
    return false;
  }

  uint32_t numLocalEntries = 0;
  if (locals.length()) {
    ValType prev = locals[0];
    numLocalEntries++;
    for (ValType t : locals) {
      if (t != prev) {
        numLocalEntries++;
        prev = t;
      }
    }
  }

  if (!e.writeVarU32(numLocalEntries)) {
    return false;
  }

  if (numLocalEntries) {
    ValType prev = locals[0];
    uint32_t count = 1;
    for (uint32_t i = 1; i < locals.length(); i++, count++) {
      if (prev != locals[i]) {
        if (!e.writeVarU32(count)) {
          return false;
        }
        if (!e.writeValType(prev)) {
          return false;
        }
        prev = locals[i];
        count = 0;
      }
    }
    if (!e.writeVarU32(count)) {
      return false;
    }
    if (!e.writeValType(prev)) {
      return false;
    }
  }

  return true;
}

bool wasm::DecodeLocalEntriesWithParams(Decoder& d,
                                        const CodeMetadata& codeMeta,
                                        uint32_t funcIndex,
                                        ValTypeVector* locals) {
  uint32_t numLocalEntries;
  if (!d.readVarU32(&numLocalEntries)) {
    return d.fail("failed to read number of local entries");
  }

  if (!locals->appendAll(codeMeta.getFuncType(funcIndex).args())) {
    return false;
  }

  for (uint32_t i = 0; i < numLocalEntries; i++) {
    uint32_t count;
    if (!d.readVarU32(&count)) {
      return d.fail("failed to read local entry count");
    }

    if (MaxLocals - locals->length() < count) {
      return d.fail("too many locals");
    }

    ValType type;
    if (!d.readValType(*codeMeta.types, codeMeta.features(), &type)) {
      return false;
    }
    if (!type.isNumber()) {
      return d.fail("bad local type");
    }

    if (!locals->appendN(type, count)) {
      return false;
    }
  }

  return true;
}


bool wasm::CheckIsSubtypeOf(Decoder& d, const CodeMetadata& codeMeta,
                            size_t opcodeOffset, ResultType subType,
                            ResultType superType) {
  if (subType.length() != superType.length()) {
    UniqueChars error(
        JS_smprintf("type mismatch: expected %zu values, got %zu values",
                    superType.length(), subType.length()));
    if (!error) {
      return false;
    }
    MOZ_ASSERT(!ResultType::isSubTypeOf(subType, superType));
    return d.fail(opcodeOffset, error.get());
  }
  for (uint32_t i = 0; i < subType.length(); i++) {
    StorageType sub = subType[i].storageType();
    StorageType super = superType[i].storageType();
    if (!CheckIsSubtypeOf(d, codeMeta, opcodeOffset, sub, super)) {
      MOZ_ASSERT(!ResultType::isSubTypeOf(subType, superType));
      return false;
    }
  }
  MOZ_ASSERT(ResultType::isSubTypeOf(subType, superType));
  return true;
}

bool wasm::CheckIsSubtypeOf(Decoder& d, const CodeMetadata& codeMeta,
                            size_t opcodeOffset, StorageType subType,
                            StorageType superType) {
  if (StorageType::isSubTypeOf(subType, superType)) {
    return true;
  }

  UniqueChars subText = ToString(subType, codeMeta.types);
  if (!subText) {
    return false;
  }

  UniqueChars superText = ToString(superType, codeMeta.types);
  if (!superText) {
    return false;
  }

  UniqueChars error(
      JS_smprintf("type mismatch: expression has type %s but expected %s",
                  subText.get(), superText.get()));
  if (!error) {
    return false;
  }

  return d.fail(opcodeOffset, error.get());
}

// Function body validation.

template <class T>
bool wasm::ValidateOps(ValidatingOpIter& iter, T& dumper,
                       const CodeMetadata& codeMeta) {
  while (true) {
    OpBytes op;
    if (!iter.readOp(&op)) {
      return false;
    }

    // End instructions get handled differently since we don't actually want to
    // dump the final `end`. Also, Else instructions need to have their
    // indentation managed when dumping.
    if (op.b0 != uint16_t(Op::End)) {
      if (op.b0 == uint64_t(Op::Else)) {
        dumper.endScope();
      }
      dumper.dumpOpBegin(op);
      if (op.b0 == uint64_t(Op::Else)) {
        dumper.startScope();
      }
    }

    Nothing nothing;
    NothingVector nothings{};
    BlockType blockType;
    ResultType resultType;

    switch (op.b0) {
      case uint16_t(Op::End): {
        LabelKind unusedKind;
        if (!iter.readEnd(&unusedKind, &resultType, &nothings, &nothings)) {
          return false;
        }
        iter.popEnd();
        if (iter.controlStackEmpty()) {
          return true;
        }

        // Only dump `end` if it was not the final `end` of the expression.
        dumper.endScope();
        dumper.dumpOpBegin(op);

        break;
      }
      case uint16_t(Op::Nop): {
        if (!iter.readNop()) {
          return false;
        }
        break;
      }
      case uint16_t(Op::Drop): {
        if (!iter.readDrop()) {
          return false;
        }
        break;
      }
      case uint16_t(Op::Call): {
        uint32_t funcIndex;
        NothingVector unusedArgs{};
        if (!iter.readCall(&funcIndex, &unusedArgs)) {
          return false;
        }
        dumper.dumpFuncIndex(funcIndex);
        break;
      }
      case uint16_t(Op::CallIndirect): {
        uint32_t funcTypeIndex, tableIndex;
        NothingVector unusedArgs{};
        if (!iter.readCallIndirect(&funcTypeIndex, &tableIndex, &nothing,
                                   &unusedArgs)) {
          return false;
        }
        dumper.dumpTableIndex(tableIndex);
        dumper.dumpTypeIndex(funcTypeIndex, /*asTypeUse=*/true);
        break;
      }
      case uint16_t(Op::I32Const): {
        int32_t constant;
        if (!iter.readI32Const(&constant)) {
          return false;
        }
        dumper.dumpI32Const(constant);
        break;
      }
      case uint16_t(Op::I64Const): {
        int64_t constant;
        if (!iter.readI64Const(&constant)) {
          return false;
        }
        dumper.dumpI64Const(constant);
        break;
      }
      case uint16_t(Op::F32Const): {
        float constant;
        if (!iter.readF32Const(&constant)) {
          return false;
        }
        dumper.dumpF32Const(constant);
        break;
      }
      case uint16_t(Op::F64Const): {
        double constant;
        if (!iter.readF64Const(&constant)) {
          return false;
        }
        dumper.dumpF64Const(constant);
        break;
      }
      case uint16_t(Op::LocalGet): {
        uint32_t localIndex;
        if (!iter.readGetLocal(&localIndex)) {
          return false;
        }
        dumper.dumpLocalIndex(localIndex);
        break;
      }
      case uint16_t(Op::LocalSet): {
        uint32_t localIndex;
        if (!iter.readSetLocal(&localIndex, &nothing)) {
          return false;
        }
        dumper.dumpLocalIndex(localIndex);
        break;
      }
      case uint16_t(Op::LocalTee): {
        uint32_t localIndex;
        if (!iter.readTeeLocal(&localIndex, &nothing)) {
          return false;
        }
        dumper.dumpLocalIndex(localIndex);
        break;
      }
      case uint16_t(Op::GlobalGet): {
        uint32_t globalIndex;
        if (!iter.readGetGlobal(&globalIndex)) {
          return false;
        }
        dumper.dumpGlobalIndex(globalIndex);
        break;
      }
      case uint16_t(Op::GlobalSet): {
        uint32_t globalIndex;
        if (!iter.readSetGlobal(&globalIndex, &nothing)) {
          return false;
        }
        dumper.dumpGlobalIndex(globalIndex);
        break;
      }
      case uint16_t(Op::SelectNumeric): {
        StackType unused;
        if (!iter.readSelect(/*typed*/ false, &unused, &nothing, &nothing,
                             &nothing)) {
          return false;
        }
        break;
      }
      case uint16_t(Op::Block): {
        if (!iter.readBlock(&blockType)) {
          return false;
        }
        dumper.dumpBlockType(blockType);
        dumper.startScope();
        break;
      }
      case uint16_t(Op::Loop): {
        if (!iter.readLoop(&blockType)) {
          return false;
        }
        dumper.dumpBlockType(blockType);
        dumper.startScope();
        break;
      }
      case uint16_t(Op::If): {
        if (!iter.readIf(&blockType, &nothing)) {
          return false;
        }
        dumper.dumpBlockType(blockType);
        dumper.startScope();
        break;
      }
      case uint16_t(Op::Else): {
        if (!iter.readElse(&resultType, &resultType, &nothings)) {
          return false;
        }
        break;
      }
      case uint16_t(Op::I32Clz):
      case uint16_t(Op::I32Ctz):
      case uint16_t(Op::I32Popcnt): {
        if (!iter.readUnary(ValType::I32, &nothing)) {
          return false;
        }
        break;
      }
      case uint16_t(Op::I64Clz):
      case uint16_t(Op::I64Ctz):
      case uint16_t(Op::I64Popcnt): {
        if (!iter.readUnary(ValType::I64, &nothing)) {
          return false;
        }
        break;
      }
      case uint16_t(Op::F32Abs):
      case uint16_t(Op::F32Neg):
      case uint16_t(Op::F32Ceil):
      case uint16_t(Op::F32Floor):
      case uint16_t(Op::F32Sqrt):
      case uint16_t(Op::F32Trunc):
      case uint16_t(Op::F32Nearest): {
        if (!iter.readUnary(ValType::F32, &nothing)) {
          return false;
        }
        break;
      }
      case uint16_t(Op::F64Abs):
      case uint16_t(Op::F64Neg):
      case uint16_t(Op::F64Ceil):
      case uint16_t(Op::F64Floor):
      case uint16_t(Op::F64Sqrt):
      case uint16_t(Op::F64Trunc):
      case uint16_t(Op::F64Nearest): {
        if (!iter.readUnary(ValType::F64, &nothing)) {
          return false;
        }
        break;
      }
      case uint16_t(Op::I32Add):
      case uint16_t(Op::I32Sub):
      case uint16_t(Op::I32Mul):
      case uint16_t(Op::I32DivS):
      case uint16_t(Op::I32DivU):
      case uint16_t(Op::I32RemS):
      case uint16_t(Op::I32RemU):
      case uint16_t(Op::I32And):
      case uint16_t(Op::I32Or):
      case uint16_t(Op::I32Xor):
      case uint16_t(Op::I32Shl):
      case uint16_t(Op::I32ShrS):
      case uint16_t(Op::I32ShrU):
      case uint16_t(Op::I32Rotl):
      case uint16_t(Op::I32Rotr): {
        if (!iter.readBinary(ValType::I32, &nothing, &nothing)) {
          return false;
        }
        break;
      }
      case uint16_t(Op::I64Add):
      case uint16_t(Op::I64Sub):
      case uint16_t(Op::I64Mul):
      case uint16_t(Op::I64DivS):
      case uint16_t(Op::I64DivU):
      case uint16_t(Op::I64RemS):
      case uint16_t(Op::I64RemU):
      case uint16_t(Op::I64And):
      case uint16_t(Op::I64Or):
      case uint16_t(Op::I64Xor):
      case uint16_t(Op::I64Shl):
      case uint16_t(Op::I64ShrS):
      case uint16_t(Op::I64ShrU):
      case uint16_t(Op::I64Rotl):
      case uint16_t(Op::I64Rotr): {
        if (!iter.readBinary(ValType::I64, &nothing, &nothing)) {
          return false;
        }
        break;
      }
      case uint16_t(Op::F32Add):
      case uint16_t(Op::F32Sub):
      case uint16_t(Op::F32Mul):
      case uint16_t(Op::F32Div):
      case uint16_t(Op::F32Min):
      case uint16_t(Op::F32Max):
      case uint16_t(Op::F32CopySign): {
        if (!iter.readBinary(ValType::F32, &nothing, &nothing)) {
          return false;
        }
        break;
      }
      case uint16_t(Op::F64Add):
      case uint16_t(Op::F64Sub):
      case uint16_t(Op::F64Mul):
      case uint16_t(Op::F64Div):
      case uint16_t(Op::F64Min):
      case uint16_t(Op::F64Max):
      case uint16_t(Op::F64CopySign): {
        if (!iter.readBinary(ValType::F64, &nothing, &nothing)) {
          return false;
        }
        break;
      }
      case uint16_t(Op::I32Eq):
      case uint16_t(Op::I32Ne):
      case uint16_t(Op::I32LtS):
      case uint16_t(Op::I32LtU):
      case uint16_t(Op::I32LeS):
      case uint16_t(Op::I32LeU):
      case uint16_t(Op::I32GtS):
      case uint16_t(Op::I32GtU):
      case uint16_t(Op::I32GeS):
      case uint16_t(Op::I32GeU): {
        if (!iter.readComparison(ValType::I32, &nothing, &nothing)) {
          return false;
        }
        break;
      }
      case uint16_t(Op::I64Eq):
      case uint16_t(Op::I64Ne):
      case uint16_t(Op::I64LtS):
      case uint16_t(Op::I64LtU):
      case uint16_t(Op::I64LeS):
      case uint16_t(Op::I64LeU):
      case uint16_t(Op::I64GtS):
      case uint16_t(Op::I64GtU):
      case uint16_t(Op::I64GeS):
      case uint16_t(Op::I64GeU): {
        if (!iter.readComparison(ValType::I64, &nothing, &nothing)) {
          return false;
        }
        break;
      }
      case uint16_t(Op::F32Eq):
      case uint16_t(Op::F32Ne):
      case uint16_t(Op::F32Lt):
      case uint16_t(Op::F32Le):
      case uint16_t(Op::F32Gt):
      case uint16_t(Op::F32Ge): {
        if (!iter.readComparison(ValType::F32, &nothing, &nothing)) {
          return false;
        }
        break;
      }
      case uint16_t(Op::F64Eq):
      case uint16_t(Op::F64Ne):
      case uint16_t(Op::F64Lt):
      case uint16_t(Op::F64Le):
      case uint16_t(Op::F64Gt):
      case uint16_t(Op::F64Ge): {
        if (!iter.readComparison(ValType::F64, &nothing, &nothing)) {
          return false;
        }
        break;
      }
      case uint16_t(Op::I32Eqz): {
        if (!iter.readConversion(ValType::I32, ValType::I32, &nothing)) {
          return false;
        }
        break;
      }
      case uint16_t(Op::I64Eqz):
      case uint16_t(Op::I32WrapI64): {
        if (!iter.readConversion(ValType::I64, ValType::I32, &nothing)) {
          return false;
        }
        break;
      }
      case uint16_t(Op::I32TruncF32S):
      case uint16_t(Op::I32TruncF32U):
      case uint16_t(Op::I32ReinterpretF32): {
        if (!iter.readConversion(ValType::F32, ValType::I32, &nothing)) {
          return false;
        }
        break;
      }
      case uint16_t(Op::I32TruncF64S):
      case uint16_t(Op::I32TruncF64U): {
        if (!iter.readConversion(ValType::F64, ValType::I32, &nothing)) {
          return false;
        }
        break;
      }
      case uint16_t(Op::I64ExtendI32S):
      case uint16_t(Op::I64ExtendI32U): {
        if (!iter.readConversion(ValType::I32, ValType::I64, &nothing)) {
          return false;
        }
        break;
      }
      case uint16_t(Op::I64TruncF32S):
      case uint16_t(Op::I64TruncF32U): {
        if (!iter.readConversion(ValType::F32, ValType::I64, &nothing)) {
          return false;
        }
        break;
      }
      case uint16_t(Op::I64TruncF64S):
      case uint16_t(Op::I64TruncF64U):
      case uint16_t(Op::I64ReinterpretF64): {
        if (!iter.readConversion(ValType::F64, ValType::I64, &nothing)) {
          return false;
        }
        break;
      }
      case uint16_t(Op::F32ConvertI32S):
      case uint16_t(Op::F32ConvertI32U):
      case uint16_t(Op::F32ReinterpretI32): {
        if (!iter.readConversion(ValType::I32, ValType::F32, &nothing)) {
          return false;
        }
        break;
      }
      case uint16_t(Op::F32ConvertI64S):
      case uint16_t(Op::F32ConvertI64U): {
        if (!iter.readConversion(ValType::I64, ValType::F32, &nothing)) {
          return false;
        }
        break;
      }
      case uint16_t(Op::F32DemoteF64): {
        if (!iter.readConversion(ValType::F64, ValType::F32, &nothing)) {
          return false;
        }
        break;
      }
      case uint16_t(Op::F64ConvertI32S):
      case uint16_t(Op::F64ConvertI32U): {
        if (!iter.readConversion(ValType::I32, ValType::F64, &nothing)) {
          return false;
        }
        break;
      }
      case uint16_t(Op::F64ConvertI64S):
      case uint16_t(Op::F64ConvertI64U):
      case uint16_t(Op::F64ReinterpretI64): {
        if (!iter.readConversion(ValType::I64, ValType::F64, &nothing)) {
          return false;
        }
        break;
      }
      case uint16_t(Op::F64PromoteF32): {
        if (!iter.readConversion(ValType::F32, ValType::F64, &nothing)) {
          return false;
        }
        break;
      }
      case uint16_t(Op::I32Load8S):
      case uint16_t(Op::I32Load8U): {
        LinearMemoryAddress<Nothing> addr;
        if (!iter.readLoad(ValType::I32, 1, &addr)) {
          return false;
        }
        dumper.dumpLinearMemoryAddress(addr);
        break;
      }
      case uint16_t(Op::I32Load16S):
      case uint16_t(Op::I32Load16U): {
        LinearMemoryAddress<Nothing> addr;
        if (!iter.readLoad(ValType::I32, 2, &addr)) {
          return false;
        }
        dumper.dumpLinearMemoryAddress(addr);
        break;
      }
      case uint16_t(Op::I32Load): {
        LinearMemoryAddress<Nothing> addr;
        if (!iter.readLoad(ValType::I32, 4, &addr)) {
          return false;
        }
        dumper.dumpLinearMemoryAddress(addr);
        break;
      }
      case uint16_t(Op::I64Load8S):
      case uint16_t(Op::I64Load8U): {
        LinearMemoryAddress<Nothing> addr;
        if (!iter.readLoad(ValType::I64, 1, &addr)) {
          return false;
        }
        dumper.dumpLinearMemoryAddress(addr);
        break;
      }
      case uint16_t(Op::I64Load16S):
      case uint16_t(Op::I64Load16U): {
        LinearMemoryAddress<Nothing> addr;
        if (!iter.readLoad(ValType::I64, 2, &addr)) {
          return false;
        }
        dumper.dumpLinearMemoryAddress(addr);
        break;
      }
      case uint16_t(Op::I64Load32S):
      case uint16_t(Op::I64Load32U): {
        LinearMemoryAddress<Nothing> addr;
        if (!iter.readLoad(ValType::I64, 4, &addr)) {
          return false;
        }
        dumper.dumpLinearMemoryAddress(addr);
        break;
      }
      case uint16_t(Op::I64Load): {
        LinearMemoryAddress<Nothing> addr;
        if (!iter.readLoad(ValType::I64, 8, &addr)) {
          return false;
        }
        dumper.dumpLinearMemoryAddress(addr);
        break;
      }
      case uint16_t(Op::F32Load): {
        LinearMemoryAddress<Nothing> addr;
        if (!iter.readLoad(ValType::F32, 4, &addr)) {
          return false;
        }
        dumper.dumpLinearMemoryAddress(addr);
        break;
      }
      case uint16_t(Op::F64Load): {
        LinearMemoryAddress<Nothing> addr;
        if (!iter.readLoad(ValType::F64, 8, &addr)) {
          return false;
        }
        dumper.dumpLinearMemoryAddress(addr);
        break;
      }
      case uint16_t(Op::I32Store8): {
        LinearMemoryAddress<Nothing> addr;
        if (!iter.readStore(ValType::I32, 1, &addr, &nothing)) {
          return false;
        }
        dumper.dumpLinearMemoryAddress(addr);
        break;
      }
      case uint16_t(Op::I32Store16): {
        LinearMemoryAddress<Nothing> addr;
        if (!iter.readStore(ValType::I32, 2, &addr, &nothing)) {
          return false;
        }
        dumper.dumpLinearMemoryAddress(addr);
        break;
      }
      case uint16_t(Op::I32Store): {
        LinearMemoryAddress<Nothing> addr;
        if (!iter.readStore(ValType::I32, 4, &addr, &nothing)) {
          return false;
        }
        dumper.dumpLinearMemoryAddress(addr);
        break;
      }
      case uint16_t(Op::I64Store8): {
        LinearMemoryAddress<Nothing> addr;
        if (!iter.readStore(ValType::I64, 1, &addr, &nothing)) {
          return false;
        }
        dumper.dumpLinearMemoryAddress(addr);
        break;
      }
      case uint16_t(Op::I64Store16): {
        LinearMemoryAddress<Nothing> addr;
        if (!iter.readStore(ValType::I64, 2, &addr, &nothing)) {
          return false;
        }
        dumper.dumpLinearMemoryAddress(addr);
        break;
      }
      case uint16_t(Op::I64Store32): {
        LinearMemoryAddress<Nothing> addr;
        if (!iter.readStore(ValType::I64, 4, &addr, &nothing)) {
          return false;
        }
        dumper.dumpLinearMemoryAddress(addr);
        break;
      }
      case uint16_t(Op::I64Store): {
        LinearMemoryAddress<Nothing> addr;
        if (!iter.readStore(ValType::I64, 8, &addr, &nothing)) {
          return false;
        }
        dumper.dumpLinearMemoryAddress(addr);
        break;
      }
      case uint16_t(Op::F32Store): {
        LinearMemoryAddress<Nothing> addr;
        if (!iter.readStore(ValType::F32, 4, &addr, &nothing)) {
          return false;
        }
        dumper.dumpLinearMemoryAddress(addr);
        break;
      }
      case uint16_t(Op::F64Store): {
        LinearMemoryAddress<Nothing> addr;
        if (!iter.readStore(ValType::F64, 8, &addr, &nothing)) {
          return false;
        }
        dumper.dumpLinearMemoryAddress(addr);
        break;
      }
      case uint16_t(Op::MemoryGrow): {
        uint32_t memoryIndex;
        if (!iter.readMemoryGrow(&memoryIndex, &nothing)) {
          return false;
        }
        dumper.dumpMemoryIndex(memoryIndex);
        break;
      }
      case uint16_t(Op::MemorySize): {
        uint32_t memoryIndex;
        if (!iter.readMemorySize(&memoryIndex)) {
          return false;
        }
        dumper.dumpMemoryIndex(memoryIndex);
        break;
      }
      case uint16_t(Op::Br): {
        uint32_t depth;
        if (!iter.readBr(&depth, &resultType, &nothings)) {
          return false;
        }
        dumper.dumpBlockDepth(depth);
        break;
      }
      case uint16_t(Op::BrIf): {
        uint32_t depth;
        if (!iter.readBrIf(&depth, &resultType, &nothings, &nothing)) {
          return false;
        }
        dumper.dumpBlockDepth(depth);
        break;
      }
      case uint16_t(Op::BrTable): {
        Uint32Vector depths;
        uint32_t defaultDepth;
        if (!iter.readBrTable(&depths, &defaultDepth, &resultType, &nothings,
                              &nothing)) {
          return false;
        }
        dumper.dumpBlockDepths(depths);
        dumper.dumpBlockDepth(defaultDepth);
        break;
      }
      case uint16_t(Op::Return): {
        if (!iter.readReturn(&nothings)) {
          return false;
        }
        break;
      }
      case uint16_t(Op::Unreachable): {
        if (!iter.readUnreachable()) {
          return false;
        }
        break;
      }
      case uint16_t(Op::MozPrefix):
        return iter.unrecognizedOpcode(&op);
      default:
        return iter.unrecognizedOpcode(&op);
    }

    dumper.dumpOpEnd();
  }

  MOZ_CRASH("unreachable");
}

template bool wasm::ValidateOps<NopOpDumper>(ValidatingOpIter& iter,
                                             NopOpDumper& dumper,
                                             const CodeMetadata& codeMeta);
template bool wasm::ValidateOps<OpDumper>(ValidatingOpIter& iter,
                                          OpDumper& dumper,
                                          const CodeMetadata& codeMeta);

// Section macros.






#ifdef ENABLE_WASM_JSPI
#endif  // ENABLE_WASM_JSPI









struct ExternType {
 private:
  DefinitionKind kind_ = DefinitionKind(-1);
  union {
    uint32_t funcTypeIndex;
    TableType tableType;
    Limits memType;
    GlobalType globalType;
    uint32_t tagFuncTypeIndex;
  };

 public:
  ExternType() : funcTypeIndex() {}

  static ExternType func(uint32_t funcTypeIndex) {
    ExternType result;
    result.kind_ = DefinitionKind::Function;
    result.funcTypeIndex = funcTypeIndex;
    return result;
  }

  static ExternType table(TableType& tableType) {
    ExternType result;
    result.kind_ = DefinitionKind::Table;
    result.tableType = tableType;
    return result;
  }

  static ExternType memory(Limits& memType) {
    ExternType result;
    result.kind_ = DefinitionKind::Memory;
    result.memType = memType;
    return result;
  }

  static ExternType global(GlobalType& globalType) {
    ExternType result;
    result.kind_ = DefinitionKind::Global;
    result.globalType = globalType;
    return result;
  }

  static ExternType tag(uint32_t tagFuncTypeIndex) {
    ExternType result;
    result.kind_ = DefinitionKind::Tag;
    result.tagFuncTypeIndex = tagFuncTypeIndex;
    return result;
  }

  DefinitionKind kind() const { return kind_; }

  uint32_t asFunc() const {
    MOZ_RELEASE_ASSERT(kind_ == DefinitionKind::Function);
    return funcTypeIndex;
  }

  const TableType& asTable() const {
    MOZ_RELEASE_ASSERT(kind_ == DefinitionKind::Table);
    return tableType;
  }

  const Limits& asMemory() const {
    MOZ_RELEASE_ASSERT(kind_ == DefinitionKind::Memory);
    return memType;
  }

  const GlobalType& asGlobal() const {
    MOZ_RELEASE_ASSERT(kind_ == DefinitionKind::Global);
    return globalType;
  }

  uint32_t asTag() const {
    MOZ_RELEASE_ASSERT(kind_ == DefinitionKind::Tag);
    return tagFuncTypeIndex;
  }
};


[[nodiscard]]
static bool AddImport(Decoder& d, CacheableName& moduleName,
                      CacheableName& itemName, ExternType importType,
                      CodeMetadata* codeMeta, ModuleMetadata* moduleMeta) {
  uint32_t importIndex = moduleMeta->imports.length();
  if (!moduleMeta->imports.emplaceBack(
          std::move(moduleName), std::move(itemName), importType.kind())) {
    return false;
  }

  switch (importType.kind()) {
    case DefinitionKind::Function: {
      if (codeMeta->funcs.length() >= MaxFuncs) {
        return d.fail("too many functions");
      }
      if (!codeMeta->funcs.append(FuncDesc(importType.asFunc()))) {
        return false;
      }
      break;
    }
    case DefinitionKind::Table: {
      if (codeMeta->numTables() >= MaxTables) {
        return d.fail("too many tables");
      }
      if (!codeMeta->tables.emplaceBack(importType.asTable(),
                                        mozilla::Nothing(),
                                        /*isImported=*/true)) {
        return false;
      }
      break;
    }
    case DefinitionKind::Memory: {
      if (codeMeta->numMemories() >= MaxMemories) {
        return d.fail("too many memories");
      }
      if (!codeMeta->memories.emplaceBack(MemoryDesc(importType.asMemory()))) {
        return false;
      }
      codeMeta->memories.back().importIndex = Some(importIndex);
      break;
    }
    case DefinitionKind::Global: {
      if (codeMeta->globals.length() >= MaxGlobals) {
        return d.fail("too many globals");
      }
      if (!codeMeta->globals.append(
              GlobalDesc(importType.asGlobal(), codeMeta->globals.length()))) {
        return false;
      }
      break;
    }
    case DefinitionKind::Tag: {
      MutableTagType tagType = js_new<TagType>();
      if (!tagType ||
          !tagType->initialize(&(*codeMeta->types)[importType.asTag()])) {
        return false;
      }
      if (codeMeta->tags.length() >= MaxTags) {
        return d.fail("too many tags");
      }
      if (!codeMeta->tags.emplaceBack(TagKind::Exception, tagType)) {
        return false;
      }
      break;
    }
    default:
      return d.fail("unsupported import kind");
  }

  return true;
}


static bool CheckImportsAgainstBuiltinModules(Decoder& d,
                                              CodeMetadata* codeMeta,
                                              ModuleMetadata* moduleMeta) {
  const BuiltinModuleIds& builtinModules = codeMeta->features().builtinModules;

  // Skip this pass if there are no builtin modules enabled
  if (builtinModules.hasNone()) {
    return true;
  }

  uint32_t importFuncIndex = 0;
  uint32_t importGlobalIndex = 0;
  for (auto& import : moduleMeta->imports) {
    Maybe<BuiltinModuleId> builtinModule =
        ImportMatchesBuiltinModule(import.module.utf8Bytes(), builtinModules);

    switch (import.kind) {
      case DefinitionKind::Function: {
        const FuncDesc& func = codeMeta->funcs[importFuncIndex];
        uint32_t funcIndex = importFuncIndex;
        importFuncIndex += 1;
        MOZ_ASSERT(codeMeta->knownFuncImports[funcIndex] ==
                   BuiltinModuleFuncId::None);

        // Skip this import if it doesn't refer to a builtin module. We do have
        // to increment the import function index regardless though.
        if (!builtinModule) {
          continue;
        }

        // Check if this import refers to a builtin module function
        const BuiltinModuleFunc* builtinFunc = nullptr;
        BuiltinModuleFuncId builtinFuncId;
        if (!ImportFieldMatchesBuiltinModuleDefinition(
                import.field.utf8Bytes(), *builtinModule,
                DefinitionKind::Function, &builtinFunc, &builtinFuncId)) {
          // Polyfillability: if the field is not found in the builtin module,
          // it will be resolved from the imports object at instantiation.
          continue;
        }

        const TypeDef& importTypeDef = (*codeMeta->types)[func.typeIndex];
        if (!TypeDef::isSubTypeOf(builtinFunc->typeDef(), &importTypeDef)) {
          return d.failf("type mismatch in %s", builtinFunc->exportName());
        }

        codeMeta->knownFuncImports[funcIndex] = builtinFuncId;
        break;
      }
      case DefinitionKind::Global: {
        const GlobalDesc& global = codeMeta->globals[importGlobalIndex];
        importGlobalIndex += 1;

        // Skip this import if it doesn't refer to a builtin module. We do have
        // to increment the import global index regardless though.
        if (!builtinModule) {
          continue;
        }

        // Only the imported string constants module has globals defined.
        if (*builtinModule != BuiltinModuleId::JSStringConstants) {
          return d.fail("unrecognized builtin module field");
        }

        // All imported globals must match a provided global type of
        // `(global (ref extern))`.
        if (global.isMutable() ||
            !ValType::isSubTypeOf(ValType(RefType::extern_().asNonNullable()),
                                  global.type())) {
          return d.failf("type mismatch");
        }

        break;
      }
      default: {
        if (!builtinModule) {
          continue;
        }
        return d.fail("unrecognized builtin import");
      }
    }
  }

  return true;
}

// A standalone validation pass that occurs after we have finished decoding all
// memories and therefore can determine if any imported builtin functions are
// invalid due to lack of memory.
static bool CheckBuiltinImportsHaveMemory(Decoder& d, CodeMetadata* codeMeta) {
  // Skip this pass if there are no builtin modules enabled.
  if (codeMeta->features().builtinModules.hasNone()) {
#ifdef DEBUG
    for (BuiltinModuleFuncId& id : codeMeta->knownFuncImports) {
      MOZ_ASSERT(id == BuiltinModuleFuncId::None);
    }
#endif
    return true;
  }

  for (size_t i = 0; i < codeMeta->knownFuncImports.length(); i++) {
    BuiltinModuleFuncId builtinFuncId = codeMeta->knownFuncImports[i];
    if (builtinFuncId == BuiltinModuleFuncId::None) {
      continue;
    }

    const BuiltinModuleFunc& builtinModuleFunc =
        BuiltinModuleFuncs::getFromId(builtinFuncId);
    if (builtinModuleFunc.usesMemory()) {
      if (codeMeta->memories.length() == 0) {
        return d.failf("func %zu is a builtin function that requires a memory",
                       i);
      }

      // NOTE(bvisness): As of today, no builtins use shared memory. If this
      // changes in the future, you will need to update this to pick up the
      // expected shared-ness from the associated builtin module. Unfortunately
      // this is currently defined exclusively by which BuiltinMemory we
      // construct in CompileBuiltinModule in WasmBuiltinModule.cpp, and there
      // is no straightforward way to map from a BuiltinModuleFuncId to a
      // BuiltinModuleId, much less to know what kind of memory it expects. It
      // would be nice to have some kind of BuiltinModuleDesc that holistically
      // describes what kind of module to construct and what functions it
      // contains, but we are not building this today :)
      if (codeMeta->memories[0].isShared()) {
        return d.fail("builtin funcs are not compatible with shared memories");
      }
    }
  }

  return true;
}


bool wasm::CheckBuiltinModuleImports(Decoder& d, CodeMetadata* codeMeta,
                                     ModuleMetadata* moduleMeta) {
  return CheckImportsAgainstBuiltinModules(d, codeMeta, moduleMeta) &&
         CheckBuiltinImportsHaveMemory(d, codeMeta);
}





using NameSet = HashSet<Span<char>, NameHasher, SystemAllocPolicy>;







// The module splitter reads the header and the section headers to find the
// code section, which is what a bytecode source is split on. It is the one
// piece of module-level reading left here: itasca reads the module, but the
// split has to happen before it can be handed one.
static bool DecodePreamble(Decoder& d, uint32_t expectedVersion) {
  if (d.bytesRemain() > MaxModuleBytes) {
    return d.fail("module too big");
  }

  uint32_t magic;
  if (!d.readFixedU32(&magic) || magic != MagicNumber) {
    return d.fail("failed to match magic number");
  }

  uint32_t version;
  if (!d.readFixedU32(&version)) {
    return d.fail("failed to read version");
  }
  if (version != expectedVersion) {
    return d.failf("binary version 0x%" PRIx32
                   " does not match expected version 0x%" PRIx32,
                   version, expectedVersion);
  }

  return true;
}

bool wasm::StartsCodeSection(const uint8_t* begin, const uint8_t* end,
                             BytecodeRange* codeSection) {
  UniqueChars unused;
  Decoder d(begin, end, 0, &unused);

  if (!DecodePreamble(d, EncodingVersionModule)) {
    return false;
  }

  while (!d.done()) {
    uint32_t headerStart = d.currentOffset();
    uint8_t id;
    BytecodeRange range;
    if (!d.readSectionHeader(&id, &range)) {
      return false;
    }

    if (id == uint8_t(SectionId::Code)) {
      if (range.size() > MaxCodeSectionBytes) {
        return false;
      }

      *codeSection = BytecodeRange(headerStart, range.end - headerStart);
      return true;
    }

    if (!d.readBytes(range.size())) {
      return false;
    }
  }

  return false;
}

#ifdef ENABLE_WASM_COMPONENTS
bool wasm::IsComponent(Decoder& d) {
  uint32_t magic;
  if (!d.readFixedU32(&magic) || magic != MagicNumber) {
    return false;
  }

  uint32_t version;
  if (!d.readFixedU32(&version)) {
    return false;
  }

  return version == EncodingVersionComponent;
}
#endif



static bool DecodeModuleNameSubsection(Decoder& d,
                                       const CustomSectionRange& nameSection,
                                       CodeMetadata* codeMeta,
                                       ModuleMetadata* moduleMeta) {
  Maybe<uint32_t> endOffset;
  if (!d.startNameSubsection(NameType::Module, &endOffset)) {
    return false;
  }
  if (!endOffset) {
    return true;
  }

  Name moduleName;
  if (!d.readVarU32(&moduleName.length)) {
    return d.fail("failed to read module name length");
  }

  MOZ_ASSERT(d.currentOffset() >= nameSection.payload.start);
  moduleName.offsetInNamePayload =
      d.currentOffset() - nameSection.payload.start;

  const uint8_t* bytes;
  if (!d.readBytes(moduleName.length, &bytes)) {
    return d.fail("failed to read module name bytes");
  }

  if (!d.finishNameSubsection(*endOffset)) {
    return false;
  }

  // Only save the module name if the whole subsection validates.
  codeMeta->nameSection->moduleName = moduleName;
  return true;
}

static bool DecodeFunctionNameSubsection(Decoder& d,
                                         const CustomSectionRange& nameSection,
                                         CodeMetadata* codeMeta,
                                         ModuleMetadata* moduleMeta) {
  Maybe<uint32_t> endOffset;
  if (!d.startNameSubsection(NameType::Function, &endOffset)) {
    return false;
  }
  if (!endOffset) {
    return true;
  }

  uint32_t nameCount = 0;
  if (!d.readVarU32(&nameCount) || nameCount > MaxFuncs) {
    return d.fail("bad function name count");
  }

  NameVector funcNames;

  for (uint32_t i = 0; i < nameCount; ++i) {
    uint32_t funcIndex = 0;
    if (!d.readVarU32(&funcIndex)) {
      return d.fail("unable to read function index");
    }

    // Names must refer to real functions and be given in ascending order.
    if (funcIndex >= codeMeta->numFuncs() || funcIndex < funcNames.length()) {
      return d.fail("invalid function index");
    }

    Name funcName;
    if (!d.readVarU32(&funcName.length) ||
        funcName.length > JS::MaxStringLength) {
      return d.fail("unable to read function name length");
    }

    if (!funcName.length) {
      continue;
    }

    if (!funcNames.resize(funcIndex + 1)) {
      return false;
    }

    MOZ_ASSERT(d.currentOffset() >= nameSection.payload.start);
    funcName.offsetInNamePayload =
        d.currentOffset() - nameSection.payload.start;

    if (!d.readBytes(funcName.length)) {
      return d.fail("unable to read function name bytes");
    }

    funcNames[funcIndex] = funcName;
  }

  if (!d.finishNameSubsection(*endOffset)) {
    return false;
  }

  // Only save names if the entire subsection decoded correctly.
  codeMeta->nameSection->funcNames = std::move(funcNames);
  return true;
}

// Decode the name section's subsections, given its payload and the index of
// the custom section it is.
//
// The name section is not part of what a module means: nothing about validity
// depends on its contents, so a validator only has to agree that its framing
// and its name are well formed. That makes this the one piece of the tail that
// stays here rather than coming from itasca.
void wasm::DecodeNameSectionPayload(const BytecodeSpan& fromPayload,
                                    uint32_t customSectionIndex,
                                    CodeMetadata* codeMeta,
                                    ModuleMetadata* moduleMeta,
                                    UniqueCharsVector* warnings) {
  const CustomSectionRange& nameSection =
      codeMeta->customSectionRanges[customSectionIndex];
  codeMeta->nameSection.emplace((NameSection){
      .customSectionIndex = customSectionIndex,
  });

  // The span runs to the end of the module rather than to the end of the
  // payload, so that a subsection reading past its section is caught and
  // reported by finishCustomSection the way it always was, rather than looking
  // like a truncated module.
  UniqueChars error;
  Decoder d(fromPayload, nameSection.payload.start, &error, warnings);

  bool ok = DecodeModuleNameSubsection(d, nameSection, codeMeta, moduleMeta) &&
            DecodeFunctionNameSubsection(d, nameSection, codeMeta, moduleMeta);
  while (ok && d.currentOffset() < nameSection.payload.end) {
    ok = d.skipNameSubsection();
  }

  // A malformed name section is a warning and not an error: nothing about the
  // module's validity depends on its contents.
  if (!d.finishCustomSection(NameSectionName, nameSection.payload)) {
    codeMeta->nameSection = mozilla::Nothing();
  }
}

static bool DecodeNameSection(Decoder& d, CodeMetadata* codeMeta,
                              ModuleMetadata* moduleMeta) {
  MaybeBytecodeRange range;
  if (!d.startCustomSection(NameSectionName, codeMeta, &range)) {
    return false;
  }
  if (!range) {
    return true;
  }

  codeMeta->nameSection.emplace((NameSection){
      .customSectionIndex =
          uint32_t(codeMeta->customSectionRanges.length() - 1),
  });
  const CustomSectionRange& nameSection = codeMeta->customSectionRanges.back();

  // Once started, custom sections do not report validation errors.

  if (!DecodeModuleNameSubsection(d, nameSection, codeMeta, moduleMeta)) {
    goto finish;
  }

  if (!DecodeFunctionNameSubsection(d, nameSection, codeMeta, moduleMeta)) {
    goto finish;
  }

  while (d.currentOffset() < range->end) {
    if (!d.skipNameSubsection()) {
      goto finish;
    }
  }

finish:
  if (!d.finishCustomSection(NameSectionName, *range)) {
    codeMeta->nameSection = mozilla::Nothing();
  }
  return true;
}


#ifdef ENABLE_WASM_COMPONENTS

// In the component model, names consist primarily of
// series-OF-possibly-UPPERCASE-fragments, where each fragment is all lowercase
// or all uppercase. A lowercase fragment is called a "word"; an uppercase
// fragment is called an "acronym". Additionally, names cannot start with
// digits. To give you a flavor, here are some of the grammar rules for names:
//
//     plainname         ::= <label>
//                         | '[constructor]' <label>
//                         | '[method]' <label> '.' <label>
//                         | '[static]' <label> '.' <label>
//     interfacename     ::= <namespace> <words> <projection> ...
//     namespace         ::= <words> ':'
//     projection        ::= '/' <label>
//
//     label             ::= <first-fragment> ( '-' <fragment> )*
//     words             ::= <first-word> ( '-' <word> )*
//
//     first-word        ::= [a-z] [0-9a-z]*
//     first-acronym     ::= [A-Z] [0-9A-Z]*
//     first-fragment    ::= <first-word>
//                         | <first-acronym>
//     word              ::= [0-9a-z]+
//     acronym           ::= [0-9A-Z]+
//     fragment          ::= <word>
//                         | <acronym>
//
// This is a maze, but at the end of the day it boils down to: parse a series of
// hyphen-separated identifiers, sometimes allowing uppercase letters (as in
// `plainname`) and sometimes not (as in `namespace`). For our own sanity, we
// just call everything a "label" in our code and explicitly indicate whether
// uppercase is allowed.
[[nodiscard]] static bool DecodeComponentLabel(Decoder& d, const char* thing,
                                               bool allowUppercase) {
  while (true) {
    uint8_t first;
    if (!d.readFixedU8(&first)) {
      return d.failf("%s name ended unexpectedly", thing);
    }
    bool firstUppercase = 'A' <= first && first <= 'Z';
    bool firstLowercase = 'a' <= first && first <= 'z';

    if (!(firstUppercase || firstLowercase)) {
      return d.failf("invalid character in %s name", thing);
    }
    if (firstUppercase && !allowUppercase) {
      return d.failf("%s name had unexpected uppercase letter", thing);
    }

    uint8_t b;
    while (d.peekByte(&b)) {
      if (b == '-') {
        break;
      }

      bool letter =
          firstUppercase ? ('A' <= b && b <= 'Z') : ('a' <= b && b <= 'z');
      bool digit = '0' <= b && b <= '9';
      if (!letter && !digit) {
        // We are immediately done because we encountered a non-word symbol at
        // the end of something that could be valid.
        return true;
      }

      MOZ_RELEASE_ASSERT(d.readBytes(1));
    }
    if (d.done()) {
      return true;
    }

    MOZ_RELEASE_ASSERT(d.readLiteral("-"));
  }
}

[[nodiscard]] static bool DecodeComponentName(Decoder& d, const char* thing,
                                              CacheableName* name,
                                              bool allowMethods) {
  uint32_t len;
  if (!d.readVarU32(&len)) {
    return d.fail("expected name");
  }
  if (len == 0) {
    return d.failf("%s name cannot be empty", thing);
  }
  if (len > d.bytesRemain()) {
    return d.fail("over-long name");
  }

  Decoder nameDecoder(d.currentPosition(), d.currentPosition() + len,
                      d.currentOffset(), d.error(), d.warnings());
  {
    Decoder& d = nameDecoder;

    // Get some unusual kinds of component names out of the way. In the future
    // we could choose to support some of these.
    if (d.peekLiteral("url=")) {
      return d.fail("URL names are not supported");
    } else if (d.peekLiteral("integrity=")) {
      return d.fail("hash names are not supported");
    } else if (d.peekLiteral("unlocked-dep=") || d.peekLiteral("locked-dep=")) {
      return d.fail("dependency names are not supported");
    }

    // Now all we have to deal with are plain names and interface names.
    // Examples of each would be:
    //
    // - Plain names: foo-BAR-baz, [constructor]FOO-BAR, [method]foo.BAR,
    //   [static]foo-BAR.BEEP-boop
    // - Interface names: wasi:cli/stdout,
    //   wasi:clocks/imports@0.3.0-rc-2026-03-15,
    //   foo-bar:BEEP-boop/boop-BEEP@<[a-zA-Z0-9.+-]+>
    //
    // For interface names, all three of namespace (e.g. "wasi:"), name (e.g.
    // "cli"), and "projection" (e.g. "/stdout") are required, while the
    // version (e.g. "@0.3.0") is optional.
    //
    // We can't distinguish up front between a plain or interface name (unless
    // there is an annotation like "[constructor]"), so parsing must be ready
    // to accommodate either.
    //
    // TODO(wasm-cm): Today we reject interface names entirely; the parser
    // does not recognize the symbols used to delimit namespaces, projections,
    // or versions.

    if (allowMethods && d.readLiteral("[constructor]")) {
      if (!DecodeComponentLabel(d, thing, /*allowUppercase=*/true)) {
        return false;
      }
    } else if (allowMethods &&
               (d.readLiteral("[method]") || d.readLiteral("[static]"))) {
      if (!DecodeComponentLabel(d, thing, /*allowUppercase=*/true)) {
        return false;
      }
      if (d.done()) {
        return d.failf("%s name ended unexpectedly", thing);
      } else if (!d.readLiteral(".")) {
        return d.failf("invalid character in %s name", thing);
      }
      if (!DecodeComponentLabel(d, thing, /*allowUppercase=*/true)) {
        return false;
      }
    } else {
      if (!DecodeComponentLabel(d, thing, /*allowUppercase=*/true)) {
        return false;
      }
    }

    if (!d.done()) {
      return d.failf("invalid characters in %s name", thing);
    }
  }

  UTF8Bytes utf8Bytes;
  if (!d.readUTF8Bytes(len, &utf8Bytes)) {
    MOZ_CRASH("full name should have been decoded earlier");
  }
  *name = CacheableName(std::move(utf8Bytes));

  return true;
}

// TODO(wasm-cm): Documentation
//
// Note that this function need not concern itself with canonicalization,
// because primitives don't need to be canonicalized and types already in the
// type section will have been canonicalized on their way in.
static bool DecodeComponentValType(Decoder& d, MutableComponent& c,
                                   ComponentType* t) {
  // Types in the binary are organized so that negative numbers are
  // primitives, while positive numbers are type indices.

  uint8_t nextByte;
  if (!d.peekByte(&nextByte)) {
    return d.fail("expected value type");
  }

  if ((nextByte & SLEB128SignMask) == SLEB128SignBit) {
    uint8_t rawKind;
    if (!d.readFixedU8(&rawKind)) {
      return false;
    }

    ComponentTypeKind primKind = ComponentTypeKind(rawKind);
    if (!ComponentTypeKindIsPrimitive(primKind)) {
      return d.failf("invalid value type 0x%02x", rawKind);
    }
    *t = ComponentType::primitive(primKind);
    return true;
  }

  int32_t typeIndex;
  if (!d.readVarS32(&typeIndex) || typeIndex < 0 ||
      c->types().length() <= size_t(typeIndex)) {
    return d.failf("invalid type index %d", typeIndex);
  }
  ComponentType referencedType = c->getType(typeIndex);
  if (!ComponentTypeKindIsValueType(referencedType.kind())) {
    return d.failf("type %d is not a value type", typeIndex);
  }
  *t = referencedType;
  return true;
}

enum class ComponentTypeKindRaw : uint8_t {
  Bool = 0x7f,
  S8 = 0x7e,
  U8 = 0x7d,
  S16 = 0x7c,
  U16 = 0x7b,
  S32 = 0x7a,
  U32 = 0x79,
  S64 = 0x78,
  U64 = 0x77,
  F32 = 0x76,
  F64 = 0x75,
  Char = 0x74,
  String = 0x73,

  Record = 0x72,
  Variant = 0x71,
  List = 0x70,
  Tuple = 0x6f,
  Flags = 0x6e,
  Enum = 0x6d,
  Option = 0x6b,
  Result = 0x6a,
  Own = 0x69,
  Borrow = 0x68,

  Func = 0x40,
  AsyncFunc = 0x43,

  Component = 0x41,
  Instance = 0x42,

  Resource = 0x3f,
};

[[nodiscard]] static bool DecodeComponentType(Decoder& d, MutableComponent& c) {
  uint8_t kind;
  if (!d.readFixedU8(&kind)) {
    return d.fail("expected type kind");
  }

  ComponentType t;
  switch (kind) {
    case uint8_t(ComponentTypeKindRaw::Bool):
    case uint8_t(ComponentTypeKindRaw::S8):
    case uint8_t(ComponentTypeKindRaw::U8):
    case uint8_t(ComponentTypeKindRaw::S16):
    case uint8_t(ComponentTypeKindRaw::U16):
    case uint8_t(ComponentTypeKindRaw::S32):
    case uint8_t(ComponentTypeKindRaw::U32):
    case uint8_t(ComponentTypeKindRaw::S64):
    case uint8_t(ComponentTypeKindRaw::U64):
    case uint8_t(ComponentTypeKindRaw::F32):
    case uint8_t(ComponentTypeKindRaw::F64):
    case uint8_t(ComponentTypeKindRaw::Char):
    case uint8_t(ComponentTypeKindRaw::String): {
      t = ComponentType::primitive(ComponentTypeKind(kind));
    } break;

    case uint8_t(ComponentTypeKindRaw::Record): {
      ComponentRecordFieldVector fields;

      // Record fields have the same name uniqueness requirement as imports.
      StronglyUniqueNameSet fieldNameDedup;

      uint32_t numFields;
      if (!d.readVarU32(&numFields)) {
        return d.fail("expected number of record fields");
      }
      if (numFields == 0) {
        return d.fail("records must have at least one field");
      }
      if (numFields > MaxComponentRecordFields) {
        return d.failf("too many record fields (max %d)",
                       MaxComponentRecordFields);
      }

      if (!fields.reserve(numFields)) {
        return false;
      }

      for (uint32_t i = 0; i < numFields; i++) {
        CacheableName name;
        if (!DecodeComponentName(d, "record field", &name,
                                 /*allowMethods=*/false)) {
          return false;
        }
        ComponentType type;
        if (!DecodeComponentValType(d, c, &type)) {
          return false;
        }

        bool duplicate;
        if (!fieldNameDedup.add(name.utf8Bytes(), &duplicate)) {
          return false;
        }
        if (duplicate) {
          return d.failf("record field name \"%.*s\" is not strongly-unique",
                         ComponentName_Printf(name));
        }
        fields.infallibleAppend(ComponentRecordField(std::move(name), type));
      }

      if (!ComponentType::record(std::move(fields), &t)) {
        return false;
      }
    } break;

    case uint8_t(ComponentTypeKindRaw::Variant): {
      ComponentVariantCaseVector cases;

      // Variant cases have the same name uniqueness requirement as imports.
      StronglyUniqueNameSet caseNameDedup;

      uint32_t numCases;
      if (!d.readVarU32(&numCases)) {
        return d.fail("expected number of variant cases");
      }
      if (numCases == 0) {
        return d.fail("variants must have at least one case");
      }
      if (numCases > MaxComponentVariantCases) {
        return d.failf("too many variant cases (max %d)",
                       MaxComponentVariantCases);
      }

      if (!cases.reserve(numCases)) {
        return false;
      }

      for (uint32_t i = 0; i < numCases; i++) {
        CacheableName name;
        bool duplicate;
        if (!DecodeComponentName(d, "variant case", &name,
                                 /*allowMethods=*/false)) {
          return false;
        }
        if (!caseNameDedup.add(name.utf8Bytes(), &duplicate)) {
          return false;
        }
        if (duplicate) {
          return d.failf("variant case name \"%.*s\" is not strongly-unique",
                         ComponentName_Printf(name));
        }

        mozilla::Maybe<ComponentType> type;
        bool hasType;
        if (!d.readBool(&hasType)) {
          return d.fail("expected optional variant case type");
        }
        if (hasType) {
          ComponentType t;
          if (!DecodeComponentValType(d, c, &t)) {
            return false;
          }
          type = mozilla::Some(t);
        }

        uint8_t dummy;
        if (!d.readFixedU8(&dummy) || dummy != 0x00) {
          return d.fail("expected trailing zero on variant case");
        }

        cases.infallibleAppend(ComponentVariantCase{std::move(name), type});
      }

      if (!ComponentType::variant(std::move(cases), &t)) {
        return false;
      }
    } break;

    case uint8_t(ComponentTypeKindRaw::List): {
      ComponentType type;
      if (!DecodeComponentValType(d, c, &type)) {
        return false;
      }
      if (!ComponentType::list(std::move(type), &t)) {
        return false;
      }
    } break;

    case uint8_t(ComponentTypeKindRaw::Tuple): {
      uint32_t numTypes;
      if (!d.readVarU32(&numTypes)) {
        return d.fail("expected number of types in tuple");
      }
      if (numTypes == 0) {
        return d.fail("tuples must have at least one type");
      }
      if (numTypes > MaxComponentTupleTypes) {
        return d.failf("too many types in tuple (max %d)",
                       MaxComponentTupleTypes);
      }

      ComponentTypeVector types;
      if (!types.reserve(numTypes)) {
        return false;
      }
      for (uint32_t i = 0; i < numTypes; i++) {
        ComponentType type;
        if (!DecodeComponentValType(d, c, &type)) {
          return false;
        }
        types.infallibleAppend(type);
      }

      if (!ComponentType::tuple(std::move(types), &t)) {
        return false;
      }
    } break;

    case uint8_t(ComponentTypeKindRaw::Flags): {
      uint32_t numLabels;
      if (!d.readVarU32(&numLabels)) {
        return false;
      }
      if (numLabels == 0) {
        return d.fail("flag type must have at least one label");
      }
      if (numLabels > MaxComponentFlagLabels) {
        return d.fail("too many labels for flag type");
      }

      CacheableNameVector labels;
      StronglyUniqueNameSet labelDedup;
      if (!labels.reserve(numLabels)) {
        return false;
      }
      for (uint32_t i = 0; i < numLabels; i++) {
        CacheableName name;
        if (!DecodeComponentName(d, "flag label", &name,
                                 /*allowMethods=*/false)) {
          return false;
        }
        bool duplicate;
        if (!labelDedup.add(name.utf8Bytes(), &duplicate)) {
          return false;
        }
        if (duplicate) {
          return d.failf("flag label \"%.*s\" is not strongly-unique",
                         ComponentName_Printf(name));
        }

        labels.infallibleAppend(std::move(name));
      }

      if (!ComponentType::flags(std::move(labels), &t)) {
        return false;
      }
    } break;

    case uint8_t(ComponentTypeKindRaw::Enum): {
      uint32_t numCases;
      if (!d.readVarU32(&numCases)) {
        return false;
      }
      if (numCases == 0) {
        return d.fail("enum must have at least one case");
      }
      if (numCases > MaxComponentEnumCases) {
        return d.failf("too many enum cases (max %d)", MaxComponentEnumCases);
      }

      CacheableNameVector labels;
      StronglyUniqueNameSet caseLabelDedup;
      if (!labels.reserve(numCases)) {
        return false;
      }
      for (uint32_t i = 0; i < numCases; i++) {
        CacheableName name;
        if (!DecodeComponentName(d, "enum case", &name,
                                 /*allowMethods=*/false)) {
          return false;
        }
        bool duplicate;
        if (!caseLabelDedup.add(name.utf8Bytes(), &duplicate)) {
          return false;
        }
        if (duplicate) {
          return d.failf("enum case label \"%.*s\" is not strongly-unique",
                         ComponentName_Printf(name));
        }

        labels.infallibleAppend(std::move(name));
      }

      if (!ComponentType::enum_(std::move(labels), &t)) {
        return false;
      }
    } break;

    case uint8_t(ComponentTypeKindRaw::Option): {
      ComponentType type;
      if (!DecodeComponentValType(d, c, &type)) {
        return false;
      }
      if (!ComponentType::option(std::move(type), &t)) {
        return false;
      }
    } break;

    case uint8_t(ComponentTypeKindRaw::Result): {
      mozilla::Maybe<ComponentType> type;
      mozilla::Maybe<ComponentType> errorType;

      bool hasType;
      if (!d.readBool(&hasType)) {
        return d.fail("expected optional result type");
      }
      if (hasType) {
        ComponentType theType;
        if (!DecodeComponentValType(d, c, &theType)) {
          return false;
        }
        type = mozilla::Some(theType);
      }

      bool hasErrorType;
      if (!d.readBool(&hasErrorType)) {
        return d.fail("expected optional result error type");
      }
      if (hasErrorType) {
        ComponentType theErrorType;
        if (!DecodeComponentValType(d, c, &theErrorType)) {
          return false;
        }
        errorType = mozilla::Some(theErrorType);
      }

      if (!ComponentType::result(
              ComponentResultType{.type = type, .errorType = errorType}, &t)) {
        return false;
      }
    } break;

    case uint8_t(ComponentTypeKindRaw::Own):
    case uint8_t(ComponentTypeKindRaw::Borrow): {
      uint32_t typeIndex;
      if (!d.readVarU32(&typeIndex)) {
        return d.fail("expected resource type index");
      }

      if (c->types().length() <= typeIndex) {
        return d.failf("invalid type index %d", typeIndex);
      }
      ComponentType rt = c->getType(typeIndex);
      if (rt.kind() != ComponentTypeKind::Resource &&
          rt.kind() != ComponentTypeKind::SubResource) {
        return d.failf("type %d is not a resource type", typeIndex);
      }

      if (kind == uint8_t(ComponentTypeKindRaw::Own)) {
        if (!ComponentType::own(std::move(rt), &t)) {
          return false;
        }
      } else {
        if (!ComponentType::borrow(std::move(rt), &t)) {
          return false;
        }
      }
    } break;

    case uint8_t(ComponentTypeKindRaw::Func):
    case uint8_t(ComponentTypeKindRaw::AsyncFunc): {
      ComponentFuncType ft;

      uint32_t numParams;
      StronglyUniqueNameSet paramDeduper;
      if (!d.readVarU32(&numParams)) {
        return d.fail("expected number of params");
      }
      if (numParams > MaxComponentParams) {
        return d.failf("too many params (max %d)", MaxComponentParams);
      }
      if (!ft.paramTypes.reserve(numParams) ||
          !ft.paramNames.reserve(numParams)) {
        return false;
      }

      for (uint32_t i = 0; i < numParams; i++) {
        CacheableName name;
        if (!DecodeComponentName(d, "param", &name,
                                 /*allowMethods=*/false)) {
          return false;
        }
        ComponentType type;
        if (!DecodeComponentValType(d, c, &type)) {
          return false;
        }

        bool duplicate;
        if (!paramDeduper.add(name.utf8Bytes(), &duplicate)) {
          return false;
        }
        if (duplicate) {
          return d.failf("param name \"%.*s\" is not strongly-unique",
                         ComponentName_Printf(name));
        }

        ft.paramNames.infallibleAppend(std::move(name));
        ft.paramTypes.infallibleAppend(std::move(type));
      }

      // There is a result type if the byte is zero. It is not clear why this
      // is, but we can only hope it is fixed when the binary format is
      // eventually reshuffled.
      bool hasNoResultType;
      if (!d.readBool(&hasNoResultType)) {
        return d.fail("expected result type");
      }
      if (hasNoResultType) {
        uint8_t dummy;
        if (!d.readFixedU8(&dummy) || dummy != 0) {
          return d.fail("expected result type");
        }
      } else {
        ComponentType resultType;
        if (!DecodeComponentValType(d, c, &resultType)) {
          return false;
        }
        ft.resultType = mozilla::Some(resultType);
      }

      if (!ComponentType::func(std::move(ft), &t)) {
        return false;
      }
    } break;

    case uint8_t(ComponentTypeKindRaw::Resource): {
      uint8_t repType;
      if (!d.readFixedU8(&repType)) {
        return d.fail("expected rep type for resource type");
      }

      // Require (rep i32)
      // TODO(wasm-cm): Update for memory64 support
      if (repType != 0x7f) {
        return d.failf("unexpected rep type 0x%02x for resource type", repType);
      }

      uint8_t hasDtor;
      mozilla::Maybe<uint32_t> dtorIndex;
      if (!d.readFixedU8(&hasDtor) || hasDtor > 0x01) {
        return d.fail("expected destructor for resource type");
      }
      if (hasDtor) {
        uint32_t dtorIndexRaw;
        if (!d.readVarU32(&dtorIndexRaw)) {
          return d.fail("expected index of destructor for resource type");
        }

        if (c->coreFuncs().length() <= dtorIndexRaw) {
          return d.failf("invalid core func index %d", dtorIndexRaw);
        }
        const FuncType& dtorType =
            c->getTypeForCoreFunc(dtorIndexRaw).funcType();

        if (!dtorType.isValidComponentDestructor()) {
          return d.fail("destructor has invalid signature");
        }

        dtorIndex.emplace(dtorIndexRaw);
      }

      if (!ComponentType::resource(ComponentResourceType(dtorIndex), &t)) {
        return false;
      }
    } break;

    default:
      return d.failf("unexpected type 0x%02x", kind);
  }

  ComponentType canonical;
  if (!CanonicalizeComponentType(t, &canonical)) {
    return false;
  }
  if (!c->addType(std::move(canonical))) {
    return false;
  }

  return true;
}

enum class ComponentSortRaw : uint8_t {
  CoreSort = 0x00,
  Function = 0x01,
  Type = 0x03,
  Component = 0x04,
  Instance = 0x05,
};

enum class ComponentCoreSortRaw : uint8_t {
  Function = 0x00,
  Table = 0x01,
  Memory = 0x02,
  Global = 0x03,
  Tag = 0x04,
  Type = 0x10,
  Module = 0x11,
  Instance = 0x12,
};

[[nodiscard]] static bool DecodeComponentCoreSort(Decoder& d,
                                                  ComponentSort* sort) {
  uint8_t coreSort;
  if (!d.readFixedU8(&coreSort)) {
    return d.fail("expected core sort");
  }

  switch (coreSort) {
    case uint8_t(ComponentCoreSortRaw::Function): {
      *sort = ComponentSort::CoreFunction;
    } break;
    case uint8_t(ComponentCoreSortRaw::Table): {
      *sort = ComponentSort::CoreTable;
    } break;
    case uint8_t(ComponentCoreSortRaw::Memory): {
      *sort = ComponentSort::CoreMemory;
    } break;
    case uint8_t(ComponentCoreSortRaw::Global): {
      *sort = ComponentSort::CoreGlobal;
    } break;
    case uint8_t(ComponentCoreSortRaw::Tag): {
      *sort = ComponentSort::CoreTag;
    } break;
    case uint8_t(ComponentCoreSortRaw::Type): {
      *sort = ComponentSort::CoreType;
    } break;
    case uint8_t(ComponentCoreSortRaw::Module): {
      *sort = ComponentSort::CoreModule;
    } break;
    case uint8_t(ComponentCoreSortRaw::Instance): {
      *sort = ComponentSort::CoreInstance;
    } break;
    default:
      return d.failf("unexpected core externtype %d", coreSort);
  }

  return true;
}

[[nodiscard]] static bool DecodeComponentSort(Decoder& d, ComponentSort* sort,
                                              bool forExterndesc) {
  uint8_t kind;
  if (!d.readFixedU8(&kind)) {
    return d.fail("expected sort");
  }

  switch (kind) {
    case uint8_t(ComponentSortRaw::CoreSort): {
      if (!DecodeComponentCoreSort(d, sort)) {
        return false;
      }
    } break;
    case uint8_t(ComponentSortRaw::Function): {
      *sort = ComponentSort::Func;
    } break;
    case uint8_t(ComponentSortRaw::Type): {
      *sort = ComponentSort::Type;
    } break;
    case uint8_t(ComponentSortRaw::Component): {
      *sort = ComponentSort::Component;
    } break;
    case uint8_t(ComponentSortRaw::Instance): {
      *sort = ComponentSort::Instance;
    } break;
    default:
      return d.failf("unexpected sort 0x%02x", kind);
  }

  if (forExterndesc && !ComponentSortValidForExternDesc(*sort)) {
    return d.failf("unexpected sort 0x%02x", kind);
  }

  return true;
}

enum class ComponentTypeBoundKindRaw : uint8_t {
  Eq = 0x00,
  SubResource = 0x01,
};

[[nodiscard]] static bool DecodeComponentExternDesc(Decoder& d,
                                                    MutableComponent c,
                                                    ComponentExternDesc* desc,
                                                    bool* isNewSubResource) {
  *isNewSubResource = false;

  ComponentSort kind;
  if (!DecodeComponentSort(d, &kind, /*forExterndesc=*/true)) {
    return false;
  }

  switch (kind) {
    case ComponentSort::Func: {
      uint32_t funcTypeIndex;
      if (!d.readVarU32(&funcTypeIndex)) {
        return d.fail("expected func type index");
      }

      if (c->types().length() <= funcTypeIndex) {
        return d.failf("invalid type index %d", funcTypeIndex);
      }
      ComponentType funcType = c->getType(funcTypeIndex);
      if (funcType.kind() != ComponentTypeKind::Func) {
        return d.failf("type %d is not a func type", funcTypeIndex);
      }

      *desc = ComponentExternDesc::func(std::move(funcType));
    } break;
    case ComponentSort::Type: {
      uint8_t kind;
      if (!d.readFixedU8(&kind)) {
        return d.fail("expected kind of type bound");
      }

      switch (kind) {
        case uint8_t(ComponentTypeBoundKindRaw::Eq): {
          uint32_t typeIndex;
          if (!d.readVarU32(&typeIndex)) {
            return d.fail("expected type index");
          }

          if (c->types().length() <= typeIndex) {
            return d.failf("invalid type index %d", typeIndex);
          }

          *desc = ComponentExternDesc::type(c->getType(typeIndex));
        } break;
        case uint8_t(ComponentTypeBoundKindRaw::SubResource): {
          // We do not need to canonicalize this new type, as all resource types
          // are unique anyway.
          ComponentType subResourceType;
          if (!ComponentType::subResource(&subResourceType)) {
            return false;
          }
          *desc = ComponentExternDesc::type(std::move(subResourceType));
          *isNewSubResource = true;
        } break;
        default:
          return d.failf("invalid kind 0x%02x for type bound", kind);
      }
    } break;
    case ComponentSort::Component: {
      // TODO(wasm-cm): Add support for these
      return d.fail("extern components are not supported yet");
    } break;
    case ComponentSort::Instance: {
      // TODO(wasm-cm): Add support for these
      return d.fail("extern instances are not supported yet");
    } break;
    case ComponentSort::CoreModule: {
      // TODO(wasm-cm): Add support for these
      return d.fail("extern core modules are not supported yet");
    } break;
    default:
      MOZ_CRASH(
          "all externdesc-compatible ComponentSorts should have been handled");
  }

  return true;
}

// Validates that a `(core:)?sortidx` from the component spec corresponds to a
// valid item in the component's index space.
[[nodiscard]] static bool ValidateComponentSortIdx(
    Decoder& d, const Component& c, ComponentSortIndex sortIndex) {
  switch (sortIndex.sort) {
    case ComponentSort::Func: {
      if (c.funcs().length() <= sortIndex.index) {
        return d.failf("invalid function index %d", sortIndex.index);
      }
    } break;
    case ComponentSort::Type: {
      if (c.types().length() <= sortIndex.index) {
        return d.failf("invalid type index %d", sortIndex.index);
      }
    } break;
    case ComponentSort::Component:
      // TODO(wasm-cm): Support nested components
      return d.fail("nested components are not supported yet");
    case ComponentSort::Instance:
      // TODO(wasm-cm): Support nested components
      return d.fail("nested components are not supported yet");

    case ComponentSort::CoreFunction: {
      if (c.coreFuncs().length() <= sortIndex.index) {
        return d.failf("invalid core function index %d", sortIndex.index);
      }
    } break;
    case ComponentSort::CoreTable: {
      if (c.coreTables().length() <= sortIndex.index) {
        return d.failf("invalid table index %d", sortIndex.index);
      }
    } break;
    case ComponentSort::CoreMemory: {
      if (c.coreMemories().length() <= sortIndex.index) {
        return d.failf("invalid memory index %d", sortIndex.index);
      }
    } break;
    case ComponentSort::CoreGlobal: {
      if (c.coreGlobals().length() <= sortIndex.index) {
        return d.failf("invalid global index %d", sortIndex.index);
      }
    } break;
    case ComponentSort::CoreTag: {
      if (c.coreTags().length() <= sortIndex.index) {
        return d.failf("invalid tag index %d", sortIndex.index);
      }
    } break;

    case ComponentSort::CoreType:
      // TODO(wasm-cm): Support core type aliases as part of outer aliases.
      return d.fail("core type aliases are not supported yet");
    case ComponentSort::CoreModule: {
      if (c.coreModules().length() <= sortIndex.index) {
        return d.failf("invalid core module index %d", sortIndex.index);
      }
    } break;
    case ComponentSort::CoreInstance: {
      if (c.coreInstances().length() <= sortIndex.index) {
        return d.failf("invalid core instance index %d", sortIndex.index);
      }
    } break;

    default:
      MOZ_CRASH();
  }

  return true;
}

enum class CoreInstanceExprKind : uint8_t {
  InstantiateModule = 0x00,
  InlineExports = 0x01,
};

[[nodiscard]] static bool DecodeCoreInstance(Decoder& d, MutableComponent& c) {
  uint8_t exprType;
  if (!d.readFixedU8(&exprType)) {
    return false;
  }

  switch (exprType) {
    case uint8_t(CoreInstanceExprKind::InstantiateModule): {
      uint32_t moduleIndex;
      if (!d.readVarU32(&moduleIndex)) {
        return d.fail("expected core module index");
      }
      if (moduleIndex >= c->coreModules().length()) {
        return d.failf("invalid core module index %d", moduleIndex);
      }

      uint32_t numArgs;
      if (!d.readVarU32(&numArgs)) {
        return d.fail("expected number of instantiate arguments");
      }
      if (numArgs > MaxComponentCoreInstantiateArgs) {
        return d.failf("too many core instantiate args (max %d)",
                       MaxComponentCoreInstantiateArgs);
      }

      CoreInstanceInstantiateArgs args;
      for (uint32_t i = 0; i < numArgs; i++) {
        CacheableName importName;
        if (!DecodeName(d, &importName)) {
          return d.fail("expected import name");
        }

        auto p = args.lookupForAdd(importName.utf8Bytes());
        if (p) {
          return d.fail("duplicate core instantiate arg name");
        }

        uint8_t instanceIndicator;
        if (!d.readFixedU8(&instanceIndicator) ||
            instanceIndicator != uint8_t(ComponentCoreSortRaw::Instance)) {
          return d.fail("expected core instance index");
        }

        uint32_t instanceIndex;
        if (!d.readVarU32(&instanceIndex)) {
          return d.fail("expected core instance index");
        }
        if (c->coreInstances().length() <= instanceIndex) {
          return d.failf("invalid core instance index %d", instanceIndex);
        }

        if (!args.add(p, std::move(importName), instanceIndex)) {
          return false;
        }
      }

      // Validate that the instantiate args satisfy the module's imports. (How
      // wonderful it is that we can do this statically for components!)
      {
        SharedModule mod = c->getCoreModule(moduleIndex);
        uint32_t functionIndex = 0;
        uint32_t tableIndex = 0;
        uint32_t memoryIndex = 0;
        uint32_t globalIndex = 0;
        uint32_t tagIndex = 0;
        for (const Import& imp : mod->moduleMeta().imports) {
          auto p = args.lookup(imp.module.utf8Bytes());
          if (!p) {
            return d.fail("no instantiate arg found");
          }
          uint32_t instanceIndex = p->value();
          const CoreInstanceDesc& instance = c->getCoreInstance(instanceIndex);
          mozilla::Maybe<ComponentSortIndex> maybeExp =
              instance.getExport(imp.field);
          if (!maybeExp) {
            return d.fail("no matching export for core module import");
          }
          ComponentSortIndex exp = *maybeExp;

          switch (imp.kind) {
            case DefinitionKind::Function: {
              if (exp.sort != ComponentSort::CoreFunction) {
                return d.failf("expected a core function");
              }

              uint32_t index = functionIndex++;
              const TypeDef& importType = mod->codeMeta().getFuncTypeDef(index);
              const TypeDef& exportType = instance.getCoreFuncType(exp.index);
              if (!TypeDef::isSubTypeOf(&exportType, &importType)) {
                return d.fail("incompatible function type for import");
              }
            } break;
            case DefinitionKind::Table: {
              if (exp.sort != ComponentSort::CoreTable) {
                return d.failf("expected a table");
              }

              uint32_t index = tableIndex++;
              const TableDesc& importTable = mod->codeMeta().tables[index];
              const TableDesc& exportTable = instance.getTable(exp.index);
              if (!TableType::matches(exportTable.type, importTable.type)) {
                return d.fail("incompatible table type for import");
              }
            } break;
            case DefinitionKind::Memory: {
              if (exp.sort != ComponentSort::CoreMemory) {
                return d.failf("expected a memory");
              }

              uint32_t index = memoryIndex++;
              MemoryDesc importMemory = mod->codeMeta().memories[index];
              MemoryDesc exportMemory = instance.getMemory(exp.index);
              if (!MemoryDesc::matches(exportMemory, importMemory)) {
                return d.fail("incompatible memory type for import");
              }
            } break;
            case DefinitionKind::Global: {
              if (exp.sort != ComponentSort::CoreGlobal) {
                return d.failf("expected a global");
              }

              uint32_t index = globalIndex++;
              const GlobalDesc& importGlobal = mod->codeMeta().globals[index];
              const GlobalDesc& exportGlobal = instance.getGlobal(exp.index);
              if (!GlobalDesc::matches(exportGlobal, importGlobal)) {
                return d.fail("incompatible global type for import");
              }
            } break;
            case DefinitionKind::Tag: {
              if (exp.sort != ComponentSort::CoreTag) {
                return d.failf("expected a tag");
              }

              uint32_t index = tagIndex++;
              const TagDesc& importTag = mod->codeMeta().tags[index];
              const TagDesc& exportTag = instance.getTag(exp.index);
              if (!TagType::matches(*exportTag.type, *importTag.type)) {
                return d.fail("incompatible tag type for import");
              }
            } break;
            default:
              MOZ_CRASH();
          }
        }
      }

      CoreInstanceDesc desc(c, CoreInstanceDescFromModule{
                                   .moduleIndex = moduleIndex,
                                   .args = std::move(args),
                               });
      if (!c->addCoreInstance(std::move(desc))) {
        return false;
      }
    } break;
    case uint8_t(CoreInstanceExprKind::InlineExports): {
      uint32_t numExports;
      if (!d.readVarU32(&numExports)) {
        return d.fail("expected number of inline exports");
      }
      if (numExports > MaxComponentInlineExports) {
        return d.failf("too many inline exports (max %d)",
                       MaxComponentInlineExports);
      }

      ComponentInlineExports inlineExports;
      ComponentInlineExports::Builder builder;
      NameSet exportNames;
      for (uint32_t i = 0; i < numExports; i++) {
        CacheableName name;
        ComponentSortIndex sortIndex;
        {
          ComponentSort sort;
          uint32_t index;
          if (!DecodeName(d, &name)) {
            return d.fail("expected inline export name");
          }
          if (!DecodeComponentCoreSort(d, &sort)) {
            return false;
          }
          if (!d.readVarU32(&index)) {
            return d.fail("expected inline export item index");
          }
          sortIndex = ComponentSortIndex(sort, index);
        }

        // Check for duplicate names
        auto p = exportNames.lookupForAdd(name.utf8Bytes());
        if (p) {
          return d.fail("duplicate name of inline export");
        }
        if (!exportNames.add(p, name.utf8Bytes())) {
          return false;
        }

        // Component-level sorts like Func and CoreModule are not allowed for
        // core instances.
        if (!ComponentSortIsCoreSort(sortIndex.sort)) {
          return d.failf("invalid sort %d for core inline export",
                         uint8_t(sortIndex.sort));
        }

        // Check that index refers to a valid item
        if (!ValidateComponentSortIdx(d, *c, sortIndex)) {
          return false;
        }

        if (!inlineExports.addExport(&builder, std::move(name), sortIndex)) {
          return false;
        }
      }

      if (!c->addCoreInstance(CoreInstanceDesc(c, std::move(inlineExports)))) {
        return false;
      }
    } break;
    default:
      return d.failf("expected type of instance expression but got %d",
                     exprType);
  }

  return true;
}

enum class AliasKindRaw : uint8_t {
  ComponentExport = 0x00,
  CoreExport = 0x01,
  Outer = 0x02,
};

[[nodiscard]] static bool DecodeComponentAlias(Decoder& d,
                                               MutableComponent& c) {
  ComponentSort sort;
  if (!DecodeComponentSort(d, &sort, /*forExterndesc=*/false)) {
    return false;
  }

  uint8_t targetType;
  if (!d.readFixedU8(&targetType)) {
    return d.fail("expected alias target");
  }

  switch (targetType) {
    case uint8_t(AliasKindRaw::ComponentExport): {
      // TODO(wasm-cm)
      return d.fail("component export aliases are not yet supported");
    } break;
    case uint8_t(AliasKindRaw::CoreExport): {
      uint32_t instanceIndex;
      if (!d.readVarU32(&instanceIndex)) {
        return d.fail("expected instance index");
      }

      CacheableName exportName;
      if (!DecodeName(d, &exportName)) {
        return d.fail("expected instance export name");
      }

      if (c->coreInstances().length() <= instanceIndex) {
        return d.failf("invalid core instance index %d", instanceIndex);
      }
      const CoreInstanceDesc& instance = c->getCoreInstance(instanceIndex);
      mozilla::Maybe<ComponentSortIndex> maybeExp =
          instance.getExport(exportName);
      if (maybeExp.isNothing()) {
        return d.failf("core instance %d has no export \"%.*s\"", instanceIndex,
                       ComponentName_Printf(exportName));
      }
      ComponentSortIndex exp = *maybeExp;

      ComponentItem newAlias = instance.desc().match(
          [&](const CoreInstanceDescFromModule&) {
            return ComponentItem::alias(ComponentAliasKind::CoreExport, sort,
                                        instanceIndex, exp.index);
          },
          [&](const ComponentInlineExports& inlineExports) {
            // If you alias an export of an inline-export instance, then you can
            // just turn the alias into an outer alias of the original thing and
            // call it a day. This saves some unnecessary indirection, prevents
            // terrible chains of aliases, and also ensures that we never have
            // to build index spaces for inline-export instances.
            //
            // TODO(wasm-cm): In principle you could also do this analysis on
            // normal modules. If a module simply re-exports an item, the same
            // trick could be performed.
            ComponentSortIndex original = inlineExports.resolveOriginal(exp);
            ComponentItem originalItem = c->resolveSortIndex(original);
            if (originalItem.isOuterAlias()) {
              // Never let an outer alias point at an outer alias; always just
              // point it at the original item.
              MOZ_ASSERT(
                  !c->resolveSortIndex(originalItem.outerAliasSortIndex())
                       .isOuterAlias());
              return originalItem;
            } else {
              return ComponentItem::alias(ComponentAliasKind::Outer,
                                          original.sort, 0, original.index);
            }
          });

      ComponentSortIndex newAliasSortIndex;
      switch (sort) {
        case ComponentSort::CoreFunction: {
          if (c->coreFuncs().length() >= MaxComponentCoreFuncs) {
            return d.failf("too many core funcs (max %d)",
                           MaxComponentCoreFuncs);
          }
          if (exp.sort != ComponentSort::CoreFunction) {
            return d.failf(
                "export \"%.*s\" of core instance %d is not a function",
                ComponentName_Printf(exportName), instanceIndex);
          }
          newAliasSortIndex = ComponentSortIndex(ComponentSort::CoreFunction,
                                                 c->coreFuncs().length());
          if (!c->addAliasOfExportedCoreFunc(newAlias)) {
            return false;
          }
        } break;
        case ComponentSort::CoreTable: {
          if (c->coreTables().length() >= MaxComponentCoreTables) {
            return d.failf("too many core tables (max %d)",
                           MaxComponentCoreTables);
          }
          if (exp.sort != ComponentSort::CoreTable) {
            return d.failf("export \"%.*s\" of core instance %d is not a table",
                           ComponentName_Printf(exportName), instanceIndex);
          }
          newAliasSortIndex = ComponentSortIndex(ComponentSort::CoreTable,
                                                 c->coreTables().length());
          if (!c->addCoreTable(newAlias)) {
            return false;
          }
        } break;
        case ComponentSort::CoreMemory: {
          if (c->coreMemories().length() >= MaxComponentCoreMemories) {
            return d.failf("too many core memories (max %d)",
                           MaxComponentCoreMemories);
          }
          if (exp.sort != ComponentSort::CoreMemory) {
            return d.failf(
                "export \"%.*s\" of core instance %d is not a memory",
                ComponentName_Printf(exportName), instanceIndex);
          }
          newAliasSortIndex = ComponentSortIndex(ComponentSort::CoreMemory,
                                                 c->coreMemories().length());
          if (!c->addCoreMemory(newAlias)) {
            return false;
          }
        } break;
        case ComponentSort::CoreGlobal: {
          if (c->coreGlobals().length() >= MaxComponentCoreGlobals) {
            return d.failf("too many core globals (max %d)",
                           MaxComponentCoreGlobals);
          }
          if (exp.sort != ComponentSort::CoreGlobal) {
            return d.failf(
                "export \"%.*s\" of core instance %d is not a global",
                ComponentName_Printf(exportName), instanceIndex);
          }
          newAliasSortIndex = ComponentSortIndex(ComponentSort::CoreGlobal,
                                                 c->coreGlobals().length());
          if (!c->addCoreGlobal(newAlias)) {
            return false;
          }
        } break;
        case ComponentSort::CoreTag: {
          if (c->coreTags().length() >= MaxComponentCoreTags) {
            return d.failf("too many core tags (max %d)", MaxComponentCoreTags);
          }
          if (exp.sort != ComponentSort::CoreTag) {
            return d.failf("export \"%.*s\" of core instance %d is not a tag",
                           ComponentName_Printf(exportName), instanceIndex);
          }
          newAliasSortIndex = ComponentSortIndex(ComponentSort::CoreTag,
                                                 c->coreTags().length());
          if (!c->addCoreTag(newAlias)) {
            return false;
          }
        } break;
        default:
          return d.failf("invalid alias sort 0x%02x", uint8_t(sort));
      }

      if (newAlias.aliasKind() == ComponentAliasKind::CoreExport &&
          !c->saveExportNameForAlias(newAliasSortIndex,
                                     std::move(exportName))) {
        return false;
      }
    } break;
    case uint8_t(AliasKindRaw::Outer): {
      // TODO(wasm-cm): Support 0-depth outer aliases
      // TODO(wasm-cm): Support all depths of outer aliases once nested
      // components are supported
      return d.fail("outer aliases are not yet supported");
    } break;
    default:
      return d.failf("unexpected alias target 0x%02x", targetType);
  }

  return true;
}

enum class CanonOptKindRaw : uint8_t {
  StringEncodingUTF8 = uint8_t(ComponentStringEncoding::UTF8),
  StringEncodingUTF16 = uint8_t(ComponentStringEncoding::UTF16),
  StringEncodingLatin1PlusUTF16 =
      uint8_t(ComponentStringEncoding::Latin1PlusUTF16),
  Memory = 0x03,
  Realloc = 0x04,
  PostReturn = 0x05,
};

// Decodes a list of canonopts in the binary format.
[[nodiscard]] static bool DecodeCanonOpts(Decoder& d, const Component& c,
                                          ComponentCanonOpts* opts,
                                          CanonMode mode) {
  *opts = {};

  uint32_t count;
  if (!d.readVarU32(&count)) {
    return d.fail("expected number of canonopts");
  }

  // TODO(wasm-cm): Do you need max canonopts when each kind of canonopt can
  // only be set once?
  if (count > MaxComponentCanonOpts) {
    return d.failf("too many canonopts (max %d)", MaxComponentCanonOpts);
  }

  bool hasStringEncoding = false;
  bool hasMemory = false;
  bool hasRealloc = false;
  bool hasPostReturn = false;
  for (uint32_t i = 0; i < count; i++) {
    uint8_t kind;
    if (!d.readFixedU8(&kind)) {
      return d.fail("expected canonopt");
    }
    switch (kind) {
      case uint8_t(CanonOptKindRaw::StringEncodingUTF8):
      case uint8_t(CanonOptKindRaw::StringEncodingUTF16):
      case uint8_t(CanonOptKindRaw::StringEncodingLatin1PlusUTF16): {
        if (hasStringEncoding) {
          return d.fail("string encoding already specified");
        }
        hasStringEncoding = true;
        opts->stringEncoding = ComponentStringEncoding(kind);
      } break;
      case uint8_t(CanonOptKindRaw::Memory): {
        if (hasMemory) {
          return d.fail("memory already specified");
        }
        hasMemory = true;
        uint32_t memoryIndex;
        if (!d.readVarU32(&memoryIndex)) {
          return d.fail("expected memory index");
        }
        if (c.coreMemories().length() <= memoryIndex) {
          return d.failf("invalid memory index %d", memoryIndex);
        }
        if (c.getCoreMemory(memoryIndex).addressType() != AddressType::I32) {
          return d.fail("memory for canonical ABI must be 32-bit");
        }
        opts->memoryIndex.emplace(memoryIndex);
      } break;
      case uint8_t(CanonOptKindRaw::Realloc): {
        if (hasRealloc) {
          return d.fail("realloc already specified");
        }
        hasRealloc = true;
        uint32_t reallocIndex;
        if (!d.readVarU32(&reallocIndex)) {
          return d.fail("expected realloc index");
        }
        if (c.coreFuncs().length() <= reallocIndex) {
          return d.failf("invalid index %d for realloc function", reallocIndex);
        }
        const FuncType& reallocType =
            c.getTypeForCoreFunc(reallocIndex).funcType();
        if (reallocType.args().length() != 4 ||
            reallocType.args()[0] != ValType::i32() ||
            reallocType.args()[1] != ValType::i32() ||
            reallocType.args()[2] != ValType::i32() ||
            reallocType.args()[3] != ValType::i32() ||
            reallocType.results().length() != 1 ||
            reallocType.results()[0] != ValType::i32()) {
          return d.fail("invalid signature for realloc function");
        }
        opts->reallocIndex.emplace(reallocIndex);
      } break;
      case uint8_t(CanonOptKindRaw::PostReturn): {
        if (mode != CanonMode::Lift) {
          return d.fail("post-return only valid for canon lift");
        }
        if (hasPostReturn) {
          return d.fail("post-return already specified");
        }
        hasPostReturn = true;
        uint32_t postReturnIndex;
        if (!d.readVarU32(&postReturnIndex)) {
          return d.fail("expected post-return index");
        }
        if (c.coreFuncs().length() <= postReturnIndex) {
          return d.failf("invalid index %d for post-return function",
                         postReturnIndex);
        }
        opts->postReturnIndex.emplace(postReturnIndex);
      } break;
      default:
        return d.failf("unexpected canonopt 0x%02x", kind);
    }
  }

  return true;
}

enum class CanonDefKindRaw : uint8_t {
  Lift = 0x00,
  Lower = 0x01,
  ResourceNew = 0x02,
  ResourceDrop = 0x03,
  ResourceRep = 0x04,
};

[[nodiscard]] static bool DecodeComponentCanonDef(Decoder& d,
                                                  MutableComponent& c) {
  uint8_t kind;
  if (!d.readFixedU8(&kind)) {
    return d.fail("expected canonical definition");
  }

  switch (kind) {
    case uint8_t(CanonDefKindRaw::Lift): {
      if (c->funcs().length() >= MaxComponentFuncs) {
        return d.failf("too many funcs (max %d)", MaxComponentFuncs);
      }

      uint8_t dummy;
      if (!d.readFixedU8(&dummy) || dummy != 0) {
        return d.fail("expected canonical definition");
      }

      uint32_t coreFuncIndex;
      if (!d.readVarU32(&coreFuncIndex)) {
        return d.fail("expected core function index");
      }
      if (c->coreFuncs().length() <= coreFuncIndex) {
        return d.failf("invalid core function index %d", coreFuncIndex);
      }

      ComponentCanonOpts opts;
      if (!DecodeCanonOpts(d, *c, &opts, CanonMode::Lift)) {
        return false;
      }

      uint32_t typeIndex;
      if (!d.readVarU32(&typeIndex)) {
        return d.fail("expected type index");
      }
      if (c->types().length() <= typeIndex) {
        return d.failf("invalid type index %d", typeIndex);
      }

      const ComponentType& t = c->getType(typeIndex);
      if (t.kind() != ComponentTypeKind::Func) {
        return d.fail("canon lift requires a func type");
      }

      const ComponentFuncType& ft = t.asFunc();
      bool memoryRequired = false;
      bool reallocRequired = false;
      bool tooDeep = false;
      mozilla::Maybe<FuncType> flattened = FlattenFuncType(
          ft, CanonMode::Lift, &memoryRequired, &reallocRequired, &tooDeep);
      if (flattened.isNothing()) {
        if (tooDeep) {
          return d.fail("flattening exceeded maximum depth");
        }
        return false;
      }

      // Because flattened func types use only primitive types, there will never
      // be any type references and a strict comparison will suffice.
      if (!FuncType::strictlyEquals(
              flattened.ref(),
              c->getTypeForCoreFunc(coreFuncIndex).funcType())) {
        return d.fail(
            "could not lift core func (component func type did not match)");
      }

      // Verify presence of (memory) and (realloc) if required.
      if (memoryRequired && opts.memoryIndex.isNothing()) {
        return d.fail("memory required for canon lift");
      }
      if (reallocRequired && opts.reallocIndex.isNothing()) {
        return d.fail("realloc required for canon lift");
      }

      // Verify post-return signature, if present. The post-return function is
      // required to have params matching the actual function's results (after
      // flattening).
      if (opts.postReturnIndex.isSome()) {
        const FuncType& postReturnType =
            c->getTypeForCoreFunc(*opts.postReturnIndex).funcType();
        if (postReturnType.args().length() != flattened->results().length() ||
            postReturnType.results().length() != 0) {
          return d.fail("invalid signature for post-return function");
        }
        for (size_t i = 0; i < postReturnType.args().length(); i++) {
          if (postReturnType.args()[i] != flattened->results()[i]) {
            return d.fail("invalid signature for post-return function");
          }
        }
      }

      if (!c->addFunc(ComponentLiftedFuncDesc(typeIndex, std::move(opts)))) {
        return false;
      }
    } break;
    case uint8_t(CanonDefKindRaw::Lower): {
      if (c->coreFuncs().length() >= MaxComponentCoreFuncs) {
        return d.failf("too many core funcs (max %d)", MaxComponentCoreFuncs);
      }

      uint8_t dummy;
      if (!d.readFixedU8(&dummy) || dummy != 0) {
        return d.fail("expected canonical definition");
      }

      uint32_t funcIndex;
      if (!d.readVarU32(&funcIndex)) {
        return d.fail("expected function index");
      }
      if (c->funcs().length() <= funcIndex) {
        return d.failf("invalid function index %d", funcIndex);
      }

      ComponentCanonOpts opts;
      if (!DecodeCanonOpts(d, *c, &opts, CanonMode::Lower)) {
        return false;
      }

      const ComponentFuncType& ft = c->getTypeForFunc(funcIndex).asFunc();
      bool memoryRequired = false;
      bool reallocRequired = false;
      bool tooDeep = false;
      mozilla::Maybe<FuncType> flattened = FlattenFuncType(
          ft, CanonMode::Lower, &memoryRequired, &reallocRequired, &tooDeep);
      if (flattened.isNothing()) {
        if (tooDeep) {
          return d.fail("flattening exceeded maximum depth");
        }
        return false;
      }
      SharedTypeDef flattenedCanonical =
          TypeContext::canonicalizeSingleType(flattened.extract());
      if (!flattenedCanonical) {
        return false;
      }

      // Verify presence of (memory) and (realloc) if required.
      if (memoryRequired && opts.memoryIndex.isNothing()) {
        return d.fail("memory required for canon lower");
      }
      if (reallocRequired && opts.reallocIndex.isNothing()) {
        return d.fail("realloc required for canon lower");
      }

      if (!c->addDefinedCoreFunc(ComponentCoreFuncDesc::lowered(
              funcIndex, std::move(flattenedCanonical)))) {
        return false;
      }
    } break;
    case uint8_t(CanonDefKindRaw::ResourceNew):
    case uint8_t(CanonDefKindRaw::ResourceDrop):
    case uint8_t(CanonDefKindRaw::ResourceRep): {
      if (c->coreFuncs().length() >= MaxComponentCoreFuncs) {
        return d.failf("too many core funcs (max %d)", MaxComponentCoreFuncs);
      }

      uint32_t resourceTypeIndex;
      if (!d.readVarU32(&resourceTypeIndex)) {
        return d.fail("expected resource type index");
      }
      if (c->types().length() <= resourceTypeIndex) {
        return d.failf("invalid type index %d", resourceTypeIndex);
      }

      ComponentItem resourceTypeItem = c->types()[resourceTypeIndex];
      ComponentType resourceType = c->getType(resourceTypeIndex);
      switch (kind) {
        case uint8_t(CanonDefKindRaw::ResourceNew):
        case uint8_t(CanonDefKindRaw::ResourceRep): {
          // resource.new and resource.rep require a resource type defined in
          // this component.
          if (resourceTypeItem.kind() != ComponentItem::ItemKind::Defined) {
            return d.fail("expected a defined resource type");
          }
          if (resourceType.kind() != ComponentTypeKind::Resource) {
            return d.fail("expected a resource type");
          }
        } break;
        case uint8_t(CanonDefKindRaw::ResourceDrop): {
          // resource.drop allows any resource type (including imported).
          if (resourceType.kind() != ComponentTypeKind::Resource &&
              resourceType.kind() != ComponentTypeKind::SubResource) {
            return d.fail("expected a resource type");
          }
        } break;
      }

      // The values for the Kind enum are chosen to align with the binary
      // format.
      ComponentResourceBuiltin builtin(ComponentResourceBuiltin::Kind(kind),
                                       resourceType);

      ValTypeVector args;
      ValTypeVector results;
      switch (builtin.kind()) {
        case ComponentResourceBuiltin::Kind::ResourceNew: {
          // TODO(wasm-cm): Update for memory64 support
          if (!args.append(ValType::i32())) {
            return false;
          }
          if (!results.append(ValType::i32())) {
            return false;
          }
        } break;
        case ComponentResourceBuiltin::Kind::ResourceDrop: {
          if (!args.append(ValType::i32())) {
            return false;
          }
        } break;
        case ComponentResourceBuiltin::Kind::ResourceRep: {
          if (!args.append(ValType::i32())) {
            return false;
          }
          // TODO(wasm-cm): Update for memory64 support
          if (!results.append(ValType::i32())) {
            return false;
          }
        } break;
        default:
          MOZ_CRASH();
      }
      SharedTypeDef coreFuncType = TypeContext::canonicalizeSingleType(
          FuncType(std::move(args), std::move(results)));

      if (!c->addDefinedCoreFunc(ComponentCoreFuncDesc::builtin(
              std::move(builtin), std::move(coreFuncType)))) {
        return false;
      }
    } break;
    default:
      return d.failf("unexpected canonical definition kind 0x%02x", kind);
  }

  return true;
}

enum class ComponentImportFlagsRaw : uint8_t {
  // Strangely, the binary encoding currently allows either 0x00 or 0x01 for the
  // flags. Both do exactly the same thing. This is supposed to be cleaned up
  // eventually.
  Plain1 = 0x00,
  Plain2 = 0x01,
  VersionSuffix = 0x02,
};

static bool DecodeComponentImport(Decoder& d, MutableComponent& c,
                                  StronglyUniqueNameSet& nameDedup) {
  uint8_t importFlags;
  if (!d.readFixedU8(&importFlags)) {
    return d.fail("expected import flags");
  }

  switch (importFlags) {
    case uint8_t(ComponentImportFlagsRaw::Plain1):
    case uint8_t(ComponentImportFlagsRaw::Plain2):
      break;
    case uint8_t(ComponentImportFlagsRaw::VersionSuffix):
      // TODO(wasm-cm): Support semver?
      return d.fail("version suffixes on imports are not allowed");
    default:
      return d.failf("invalid import flags %#x", importFlags);
  }

  CacheableName importName;
  if (!DecodeComponentName(d, "import", &importName,
                           /*allowMethods=*/true)) {
    return false;
  }

  ComponentExternDesc externDesc;
  bool unused;
  if (!DecodeComponentExternDesc(d, c, &externDesc, &unused)) {
    return false;
  }
  if (externDesc.sort() == ComponentSort::Type) {
    ComponentType t = externDesc.asType();
    if (t.kind() == ComponentTypeKind::Resource) {
      return d.fail("cannot import a type equal to a defined resource type");
    }
  }

  bool duplicate;
  if (!nameDedup.add(importName.utf8Bytes(), &duplicate)) {
    return false;
  }
  if (duplicate) {
    return d.failf("import name \"%.*s\" is not strongly-unique",
                   ComponentName_Printf(importName));
  }

  return c->addImport(ComponentImport(std::move(importName), externDesc));
}

enum class ComponentExportFlagsRaw : uint8_t {
  // As with imports, 0x00 and 0x01 are equivalent flags For Now.
  Plain1 = 0x00,
  Plain2 = 0x01,
  VersionSuffix = 0x02,
};

[[nodiscard]] static bool DecodeComponentExport(
    Decoder& d, MutableComponent& c, StronglyUniqueNameSet& nameDedup) {
  uint8_t exportFlags;
  if (!d.readFixedU8(&exportFlags)) {
    return d.fail("expected export flags");
  }

  switch (exportFlags) {
    case uint8_t(ComponentImportFlagsRaw::Plain1):
    case uint8_t(ComponentImportFlagsRaw::Plain2):
      break;
    case uint8_t(ComponentImportFlagsRaw::VersionSuffix):
      // TODO(wasm-cm): Support semver?
      return d.fail("version suffixes on exports are not allowed");
    default:
      return d.failf("invalid export flags %#x", exportFlags);
  }

  CacheableName exportName;
  if (!DecodeComponentName(d, "export", &exportName,
                           /*allowMethods=*/true)) {
    return false;
  }

  ComponentSortIndex exported;
  {
    ComponentSort exportSort;
    if (!DecodeComponentSort(d, &exportSort, /*forExterndesc=*/true)) {
      return false;
    }
    uint32_t exportIndex;
    if (!d.readVarU32(&exportIndex)) {
      return d.fail("expected export index");
    }
    exported = ComponentSortIndex(exportSort, exportIndex);
  }

  if (!ValidateComponentSortIdx(d, *c, exported)) {
    return false;
  }

  ComponentExternDesc externDesc;
  switch (exported.sort) {
    case ComponentSort::Func: {
      externDesc = ComponentExternDesc::func(c->getTypeForFunc(exported.index));
    } break;
    case ComponentSort::Type: {
      externDesc = ComponentExternDesc::type(c->getType(exported.index));
    } break;
    case ComponentSort::Component: {
      // TODO(wasm-cm): Support all export sorts
      return d.fail("exported components are not supported yet");
    } break;
    case ComponentSort::Instance: {
      // TODO(wasm-cm): Support all export sorts
      return d.fail("exported component instances are not supported yet");
    } break;
    case ComponentSort::CoreModule: {
      externDesc = ComponentExternDesc::coreModule(exported.index);
    } break;
    default:
      MOZ_CRASH("all cases from DecodeComponentSort should have been handled");
  }

  uint8_t hasExplicitExternDesc;
  if (!d.readFixedU8(&hasExplicitExternDesc) || hasExplicitExternDesc > 0x01) {
    return d.fail("expected possible explicit external type");
  }
  if (hasExplicitExternDesc) {
    ComponentExternDesc explicitExternDesc;
    bool isNewSubResource;
    if (!DecodeComponentExternDesc(d, c, &explicitExternDesc,
                                   &isNewSubResource)) {
      return false;
    }

    if (!ComponentExternDesc::compatible(externDesc, explicitExternDesc,
                                         isNewSubResource)) {
      return d.fail(
          "exported item's type did not match explicitly-provided type");
    }
    externDesc = explicitExternDesc;
  }

  // TODO(wasm-cm): Validate that all resource types used (transitively!) in the
  // exported thing's type came from a preceding import or were previously
  // exported. (From talking with Luke, it sounds like actually some (but not
  // all) value types are considered "tricky" enough to fall under this
  // restriction as well, including e.g. records but excluding e.g. s32. What is
  // this list? Who knows.)

  // TODO(wasm-cm): Validate all the naming-related conditions

  bool duplicate;
  if (!nameDedup.add(exportName.utf8Bytes(), &duplicate)) {
    return false;
  }
  if (duplicate) {
    return d.failf("export name \"%.*s\" is not strongly-unique",
                   ComponentName_Printf(exportName));
  }

  return c->addExport(ComponentExport(std::move(exportName), externDesc));
}

[[nodiscard]] static bool DecodeComponentCoreModuleSection(
    Decoder& d, MutableComponent& c, const BytecodeSpan& moduleBytes,
    const CompileArgs& args, JS::OptimizedEncodingListener* listener) {
  if (c->coreModules().length() >= MaxComponentCoreModules) {
    return d.failf("too many core modules (max %d)", MaxComponentCoreModules);
  }

  BytecodeSource moduleSource(moduleBytes.data(), moduleBytes.size());
  SharedModule module =
      CompileModule(args, BytecodeBufferOrSource(moduleSource), d.error(),
                    d.warnings(), listener);
  if (!module) {
    return false;
  }
  if (!c->addCoreModule(module)) {
    return false;
  }

  MOZ_RELEASE_ASSERT(d.readBytes(moduleBytes.Length()));

  return true;
}

[[nodiscard]] static bool DecodeComponentCoreInstanceSection(
    Decoder& d, MutableComponent& c) {
  uint32_t numInstances;
  if (!d.readVarU32(&numInstances)) {
    return d.fail("expected number of instances");
  }
  if (c->coreInstances().length() + uint64_t(numInstances) >
      MaxComponentCoreInstances) {
    return d.failf("too many core instances (max %d)",
                   MaxComponentCoreInstances);
  }

  for (uint32_t i = 0; i < numInstances; i++) {
    if (!DecodeCoreInstance(d, c)) {
      return false;
    }
  }

  return true;
}

[[nodiscard]] static bool DecodeComponentAliasSection(Decoder& d,
                                                      MutableComponent& c) {
  uint32_t numAliases;
  if (!d.readVarU32(&numAliases)) {
    return d.fail("expected number of aliases");
  }
  // We do not check an implementation limit here because each alias
  // adds entries to a different index space with its own limit.

  for (uint32_t i = 0; i < numAliases; i++) {
    if (!DecodeComponentAlias(d, c)) {
      return false;
    }
  }

  return true;
}

[[nodiscard]] static bool DecodeComponentTypeSection(Decoder& d,
                                                     MutableComponent& c) {
  uint32_t numTypes;
  if (!d.readVarU32(&numTypes)) {
    return d.fail("expected number of types");
  }
  if (c->types().length() + uint64_t(numTypes) > MaxComponentTypes) {
    return d.failf("too many types (max %d)", MaxComponentTypes);
  }

  for (uint32_t i = 0; i < numTypes; i++) {
    if (!DecodeComponentType(d, c)) {
      return false;
    }
  }

  return true;
}

[[nodiscard]] static bool DecodeComponentCanonSection(Decoder& d,
                                                      MutableComponent& c) {
  uint32_t numCanonDefs;
  if (!d.readVarU32(&numCanonDefs)) {
    return d.fail("expected number of canonical definitions");
  }
  // Implementation limits are checked in DecodeComponentCanonDef.

  for (uint32_t i = 0; i < numCanonDefs; i++) {
    if (!DecodeComponentCanonDef(d, c)) {
      return false;
    }
  }

  return true;
}

[[nodiscard]] static bool DecodeComponentImportSection(
    Decoder& d, MutableComponent& c, StronglyUniqueNameSet& nameDedup) {
  uint32_t numImports;
  if (!d.readVarU32(&numImports)) {
    return d.fail("expected number of imports");
  }
  if (c->imports().length() + uint64_t(numImports) > MaxComponentImports) {
    return d.failf("too many imports (max %d)", MaxComponentImports);
  }

  for (uint32_t i = 0; i < numImports; i++) {
    if (!DecodeComponentImport(d, c, nameDedup)) {
      return false;
    }
  }

  return true;
}

[[nodiscard]] static bool DecodeComponentExportSection(
    Decoder& d, MutableComponent& c, StronglyUniqueNameSet& nameDedup) {
  uint32_t numExports;
  if (!d.readVarU32(&numExports)) {
    return d.fail("expected number of exports");
  }
  if (c->exports().length() + uint64_t(numExports) > MaxComponentExports) {
    return d.failf("too many exports (max %d)", MaxComponentExports);
  }

  for (uint32_t i = 0; i < numExports; i++) {
    if (!DecodeComponentExport(d, c, nameDedup)) {
      return false;
    }
  }

  return true;
}

bool wasm::DecodeComponent(Decoder& d, MutableComponent c,
                           const CompileArgs& args,
                           JS::OptimizedEncodingListener* listener) {
  if (!DecodePreamble(d, EncodingVersionComponent)) {
    return false;
  }

  StronglyUniqueNameSet importNameDedup;
  StronglyUniqueNameSet exportNameDedup;

  while (!d.done()) {
    uint8_t sectionID;
    if (!d.readFixedU8(&sectionID)) {
      return d.fail("expected section ID");
    }

    uint32_t sectionLength;
    if (!d.readVarU32(&sectionLength)) {
      return d.fail("expected section length");
    }

    BytecodeSpan sectionBytes;
    size_t sectionOffset;
    if (!d.readBytesSpan(sectionLength, &sectionBytes, &sectionOffset)) {
      return d.failf("invalid section length: expected %" PRIu64
                     " bytes, but only %" PRIu64 " remain",
                     uint64_t(sectionLength), uint64_t(d.bytesRemain()));
    }

    // Decode the section with its own decoder.
    Decoder sectionDecoder(sectionBytes, sectionOffset, d.error(),
                           d.warnings());
    {
      Decoder& d = sectionDecoder;

      switch (sectionID) {
        case uint8_t(ComponentSectionId::Custom): {
          if (!d.readBytes(sectionLength)) {
            return d.fail("expected custom section");
          }

          // TODO(wasm-cm): Parse custom section name, warn if it is "malformed"
          // TODO(wasm-cm): Parse component name section
        } break;

        case uint8_t(ComponentSectionId::CoreModule): {
          if (!DecodeComponentCoreModuleSection(d, c, sectionBytes, args,
                                                listener)) {
            return false;
          }
        } break;
        case uint8_t(ComponentSectionId::CoreInstance): {
          if (!DecodeComponentCoreInstanceSection(d, c)) {
            return false;
          }
        } break;
        case uint8_t(ComponentSectionId::Alias): {
          if (!DecodeComponentAliasSection(d, c)) {
            return false;
          }
        } break;
        case uint8_t(ComponentSectionId::Type): {
          if (!DecodeComponentTypeSection(d, c)) {
            return false;
          }
        } break;
        case uint8_t(ComponentSectionId::Canon): {
          if (!DecodeComponentCanonSection(d, c)) {
            return false;
          }
        } break;
        case uint8_t(ComponentSectionId::Import): {
          if (!DecodeComponentImportSection(d, c, importNameDedup)) {
            return false;
          }
        } break;
        case uint8_t(ComponentSectionId::Export): {
          if (!DecodeComponentExportSection(d, c, exportNameDedup)) {
            return false;
          }
        } break;

        default: {
          return d.failf("unexpected section ID %d", sectionID);
        }
      }

      if (!d.done()) {
        return d.failf("too many bytes in section (%zu extra)",
                       d.bytesRemain());
      }
    }
  }

  return true;
}

#endif  // ENABLE_WASM_COMPONENTS

// Validate algorithm.

#ifdef ENABLE_WASM_COMPONENTS
[[nodiscard]] static bool ValidateComponent(JSContext* cx,
                                            const BytecodeSource& bytecode,
                                            const FeatureOptions& options,
                                            UniqueChars* error) {
  MutableComponent c = js_new<Component>();
  if (!c) {
    return false;
  }

  CompileArgsError compileArgsError;
  SharedCompileArgs compileArgs =
      CompileArgs::build(cx, ScriptedCaller(), options, &compileArgsError);
  if (!compileArgs) {
    return false;
  }
  Decoder d(bytecode.envSpan(), bytecode.envRange().start, error);
  if (!DecodeComponent(d, c, *compileArgs)) {
    return false;
  }

  MOZ_ASSERT(!*error, "unreported error in decoding");
  return true;
}
#endif  // ENABLE_WASM_COMPONENTS

bool wasm::Validate(JSContext* cx, const BytecodeSource& bytecode,
                    const FeatureOptions& options, UniqueChars* error) {
#ifdef ENABLE_WASM_COMPONENTS
  FeatureArgs features = FeatureArgs::build(cx, options);
  if (features.components) {
    Decoder preambleDecoder(bytecode.envSpan(), bytecode.envRange().start,
                            error);
    if (IsComponent(preambleDecoder)) {
      return ValidateComponent(cx, bytecode, options, error);
    }
  }
#endif

  // A module's verdict is itasca's, plus the implementation caps itasca
  // does not carry. Nothing here builds metadata: validate() only reports a
  // bool, and the caps are what keep it in step with compilation.
  return ValidateWithItasca(bytecode, error);
}
