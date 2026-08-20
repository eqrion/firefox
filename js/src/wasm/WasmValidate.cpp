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

bool wasm::ValidateFunctionBody(const CodeMetadata& codeMeta,
                                uint32_t funcIndex, uint32_t bodySize,
                                Decoder& d) {
  const uint8_t* bodyBegin = d.currentPosition();
  const uint8_t* bodyEnd = bodyBegin + bodySize;

  ValTypeVector locals;
  if (!DecodeLocalEntriesWithParams(d, codeMeta, funcIndex, &locals)) {
    return false;
  }

  ValidatingOpIter iter(codeMeta, d, locals);
  NopOpDumper visitor;

  if (!iter.startFunction(funcIndex)) {
    return false;
  }

  if (!ValidateOps(iter, visitor, codeMeta)) {
    return false;
  }

  return iter.endFunction(bodyEnd);
}

// Section macros.

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

static bool DecodeValTypeVector(Decoder& d, CodeMetadata* codeMeta,
                                uint32_t count, ValTypeVector* valTypes) {
  if (!valTypes->resize(count)) {
    return false;
  }

  for (uint32_t i = 0; i < count; i++) {
    if (!d.readValType(*codeMeta->types, codeMeta->features(),
                       &(*valTypes)[i])) {
      return false;
    }
    if (!(*valTypes)[i].isNumber()) {
      return d.fail("bad type");
    }
  }
  return true;
}

static bool DecodeFuncType(Decoder& d, CodeMetadata* codeMeta,
                           FuncType* funcType) {
  uint32_t numArgs;
  if (!d.readVarU32(&numArgs)) {
    return d.fail("bad number of function args");
  }
  if (numArgs > MaxParams) {
    return d.fail("too many arguments in signature");
  }
  ValTypeVector args;
  if (!DecodeValTypeVector(d, codeMeta, numArgs, &args)) {
    return false;
  }

  uint32_t numResults;
  if (!d.readVarU32(&numResults)) {
    return d.fail("bad number of function returns");
  }
  if (numResults > 1) {
    return d.fail("too many returns in signature");
  }
  ValTypeVector results;
  if (!DecodeValTypeVector(d, codeMeta, numResults, &results)) {
    return false;
  }

  *funcType = FuncType(std::move(args), std::move(results));
  return true;
}

static bool DecodeStructType(Decoder& d, CodeMetadata* codeMeta,
                             StructType* structType) {
  uint32_t numFields;
  if (!d.readVarU32(&numFields)) {
    return d.fail("Bad number of fields");
  }

  if (numFields > MaxStructFields) {
    return d.fail("too many fields in struct");
  }

  FieldTypeVector fields;
  if (!fields.resize(numFields)) {
    return false;
  }

  for (uint32_t i = 0; i < numFields; i++) {
    if (!d.readStorageType(*codeMeta->types, codeMeta->features(),
                           &fields[i].type)) {
      return false;
    }

    uint8_t flags;
    if (!d.readFixedU8(&flags)) {
      return d.fail("expected flag");
    }
    if ((flags & ~uint8_t(FieldFlags::AllowedMask)) != 0) {
      return d.fail("garbage flag bits");
    }
    fields[i].isMutable = flags & uint8_t(FieldFlags::Mutable);
  }

  *structType = StructType(std::move(fields));

  // Compute the struct layout, and fail if the struct is too large
  if (!structType->init()) {
    return d.fail("too many fields in struct");
  }
  return true;
}

static bool DecodeArrayType(Decoder& d, CodeMetadata* codeMeta,
                            ArrayType* arrayType) {
  StorageType elementType;
  if (!d.readStorageType(*codeMeta->types, codeMeta->features(),
                         &elementType)) {
    return false;
  }

  uint8_t flags;
  if (!d.readFixedU8(&flags)) {
    return d.fail("expected flag");
  }
  if ((flags & ~uint8_t(FieldFlags::AllowedMask)) != 0) {
    return d.fail("garbage flag bits");
  }
  bool isMutable = flags & uint8_t(FieldFlags::Mutable);

  *arrayType = ArrayType(elementType, isMutable);
  return true;
}

#ifdef ENABLE_WASM_JSPI
static bool DecodeContType(Decoder& d, CodeMetadata* codeMeta,
                           ContType* contType) {
  uint32_t typeIndex;
  if (!d.readTypeIndex(&typeIndex)) {
    return false;
  }

  if (typeIndex >= codeMeta->types->length()) {
    return d.fail("type index out of range");
  }

  // We don't validate that a continuation points at a function type until we've
  // decoded the whole recursion group.
  const TypeDef& typeDef = codeMeta->types->type(typeIndex);
  *contType = ContType(&typeDef);
  return true;
}
#endif  // ENABLE_WASM_JSPI

static bool DecodeTypeSection(Decoder& d, CodeMetadata* codeMeta) {
  MaybeBytecodeRange range;
  if (!d.startSection(SectionId::Type, codeMeta, &range, "type")) {
    return false;
  }
  if (!range) {
    return true;
  }

  uint32_t numRecGroups;
  if (!d.readVarU32(&numRecGroups)) {
    return d.fail("expected number of types");
  }

  // Check if we've reached our implementation defined limit of recursion
  // groups.
  if (numRecGroups > MaxRecGroups) {
    return d.fail("too many types");
  }

  for (uint32_t recGroupIndex = 0; recGroupIndex < numRecGroups;
       recGroupIndex++) {
    uint32_t recGroupLength = 1;

    uint8_t firstTypeCode;
    if (!d.peekByte(&firstTypeCode)) {
      return d.fail("expected type form");
    }

    if (firstTypeCode == (uint8_t)TypeCode::RecGroup) {
      // Skip over the prefix byte that was peeked.
      d.uncheckedReadFixedU8();

      // Read the number of types in this recursion group
      if (!d.readVarU32(&recGroupLength)) {
        return d.fail("expected recursion group length");
      }
    }

    // Check if we've reached our implementation defined limit of type
    // definitions.
    mozilla::CheckedUint32 newNumTypes(codeMeta->types->length());
    newNumTypes += recGroupLength;
    if (!newNumTypes.isValid() || newNumTypes.value() > MaxTypes) {
      return d.fail("too many types");
    }

    // Start a recursion group. This will extend the type context with empty
    // type definitions to be filled.
    MutableRecGroup recGroup = codeMeta->types->startRecGroup(recGroupLength);
    if (!recGroup) {
      return false;
    }

    // Cancel the pending rec group if we return early due to a validation
    // error, so that the bad group is not left in recGroups_ for ~TypeContext
    // to hash during cleanup.
    auto cancelRecGroup =
        mozilla::MakeScopeExit([&] { codeMeta->types->cancelStartRecGroup(); });

    // First, iterate over the types, validate them and set super types.
    // Subtyping relationship will be checked in a second iteration.
    for (uint32_t recGroupTypeIndex = 0; recGroupTypeIndex < recGroupLength;
         recGroupTypeIndex++) {
      uint32_t typeIndex =
          codeMeta->types->length() - recGroupLength + recGroupTypeIndex;

      // This is ensured by above
      MOZ_ASSERT(typeIndex < MaxTypes);

      uint8_t form;
      const TypeDef* superTypeDef = nullptr;

      // By default, all types are final unless the sub keyword is specified.
      bool finalTypeFlag = true;

      // Decode an optional declared super type index.
      if (d.peekByte(&form) && (form == (uint8_t)TypeCode::SubNoFinalType ||
                                form == (uint8_t)TypeCode::SubFinalType)) {
        if (form == (uint8_t)TypeCode::SubNoFinalType) {
          finalTypeFlag = false;
        }

        // Skip over the `sub` or `final` prefix byte we peeked.
        d.uncheckedReadFixedU8();

        // Decode the number of super types, which is currently limited to at
        // most one.
        uint32_t numSuperTypes;
        if (!d.readVarU32(&numSuperTypes)) {
          return d.fail("expected number of super types");
        }
        if (numSuperTypes > 1) {
          return d.fail("too many super types");
        }

        // Decode the super type, if any.
        if (numSuperTypes == 1) {
          uint32_t superTypeDefIndex;
          if (!d.readVarU32(&superTypeDefIndex)) {
            return d.fail("expected super type index");
          }

          // A super type index must be strictly less than the current type
          // index in order to avoid cycles.
          if (superTypeDefIndex >= typeIndex) {
            return d.fail("invalid super type index");
          }

          superTypeDef = &codeMeta->types->type(superTypeDefIndex);
        }
      }

      // Decode the kind of type definition
      if (!d.readFixedU8(&form)) {
        return d.fail("expected type form");
      }

      TypeDef* typeDef = &recGroup->type(recGroupTypeIndex);
      switch (form) {
        case uint8_t(TypeCode::Func): {
          FuncType funcType;
          if (!DecodeFuncType(d, codeMeta, &funcType)) {
            return false;
          }
          *typeDef = std::move(funcType);
          break;
        }
        case uint8_t(TypeCode::Struct): {
          StructType structType;
          if (!DecodeStructType(d, codeMeta, &structType)) {
            return false;
          }
          *typeDef = std::move(structType);
          break;
        }
        case uint8_t(TypeCode::Array): {
          ArrayType arrayType;
          if (!DecodeArrayType(d, codeMeta, &arrayType)) {
            return false;
          }
          *typeDef = std::move(arrayType);
          break;
        }
#ifdef ENABLE_WASM_JSPI
        case uint8_t(TypeCode::Cont): {
          if (!codeMeta->stackSwitchingEnabled()) {
            return d.fail("stack switching is not enabled");
          }
          ContType contType;
          if (!DecodeContType(d, codeMeta, &contType)) {
            return false;
          }
          *typeDef = std::move(contType);
          break;
        }
#endif  // ENABLE_WASM_JSPI
        default:
          return d.fail("expected type form");
      }

      typeDef->setFinal(finalTypeFlag);
      if (superTypeDef) {
        // Check that we aren't creating too deep of a subtyping chain
        if (superTypeDef->subTypingDepth() >= MaxSubTypingDepth) {
          return d.fail("type is too deep");
        }

        typeDef->setSuperTypeDef(superTypeDef);
      }

      if (typeDef->isFuncType()) {
        typeDef->funcType().initImmediateTypeId(typeDef->isFinal(),
                                                superTypeDef, recGroupLength);
      }
    }

#ifdef ENABLE_WASM_JSPI
    // Continuation types must refer to function types. We can only validate
    // this after we've decoded all the types in the recursion group though.
    for (uint32_t recGroupTypeIndex = 0; recGroupTypeIndex < recGroupLength;
         recGroupTypeIndex++) {
      TypeDef* typeDef = &recGroup->type(recGroupTypeIndex);
      if (!typeDef->isContType()) {
        continue;
      }

      if (!typeDef->contType().funcTypeDef().isFuncType()) {
        return d.fail("cont must reference a func type");
      }
    }
#endif  // ENABLE_WASM_JSPI

    // Check the super types to make sure they are compatible with their
    // subtypes. This is done in a second iteration to avoid dealing with not
    // yet loaded types.
    for (uint32_t recGroupTypeIndex = 0; recGroupTypeIndex < recGroupLength;
         recGroupTypeIndex++) {
      TypeDef* typeDef = &recGroup->type(recGroupTypeIndex);
      if (typeDef->superTypeDef()) {
        // Check that the super type is compatible with this type
        if (!TypeDef::canBeSubTypeOf(typeDef, typeDef->superTypeDef())) {
          return d.fail("incompatible super type");
        }
      }
    }

    // Finish the recursion group, which will canonicalize the types.
    cancelRecGroup.release();
    if (!codeMeta->types->endRecGroup()) {
      return false;
    }
  }

  return d.finishSection(*range, "type");
}

[[nodiscard]] static bool DecodeName(Decoder& d, CacheableName* name) {
  uint32_t numBytes;
  if (!d.readVarU32(&numBytes)) {
    return false;
  }

  UTF8Bytes utf8Bytes;
  if (!d.readUTF8Bytes(numBytes, &utf8Bytes)) {
    return false;
  }
  *name = CacheableName(std::move(utf8Bytes));
  return true;
}

static bool DecodeFuncTypeIndex(Decoder& d, const SharedTypeContext& types,
                                uint32_t* funcTypeIndex) {
  if (!d.readVarU32(funcTypeIndex)) {
    return d.fail("expected signature index");
  }

  if (*funcTypeIndex >= types->length()) {
    return d.fail("signature index out of range");
  }

  const TypeDef& def = (*types)[*funcTypeIndex];

  if (!def.isFuncType()) {
    return d.fail("signature index references non-signature");
  }

  return true;
}

// https://webassembly.github.io/spec/core/binary/types.html#limits
//
// wasm 1.0 limits are always 32-bit and never shared; the only flag bit is
// whether a maximum is present.
static bool DecodeLimits(Decoder& d, const CodeMetadata* codeMeta,
                         LimitsKind kind, Limits* limits) {
  uint8_t flags;
  if (!d.readFixedU8(&flags)) {
    return d.fail("expected flags");
  }

  if (flags & ~uint8_t(LimitsFlags::HasMaximum)) {
    return d.failf("unexpected bits set in flags: %" PRIu32,
                   uint32_t(flags & ~uint8_t(LimitsFlags::HasMaximum)));
  }

  limits->shared = Shareable::False;
  limits->addressType = AddressType::I32;
  limits->pageSize = PageSize::Standard;

  uint32_t initial;
  if (!d.readVarU32(&initial)) {
    return d.fail("expected initial length");
  }
  limits->initial = initial;

  if (flags & uint8_t(LimitsFlags::HasMaximum)) {
    uint32_t maximum;
    if (!d.readVarU32(&maximum)) {
      return d.fail("expected maximum length");
    }

    if (limits->initial > maximum) {
      return d.failf(
          "%s size minimum must not be greater than maximum; "
          "maximum length %" PRIu32 " is less than initial length %" PRIu64,
          kind == LimitsKind::Memory ? "memory" : "table", maximum,
          limits->initial);
    }

    limits->maximum.emplace(maximum);
  }

  return true;
}

// https://webassembly.github.io/spec/core/binary/types.html#table-types
//
// wasm 1.0 tables are always `funcref`, so the element type isn't just
// restricted to funcref here, it's implied by the grammar.
static bool DecodeTableType(Decoder& d, const CodeMetadata* codeMeta,
                            TableType* tableType) {
  if (!d.readRefType(*codeMeta->types, codeMeta->features(),
                     &tableType->elemType)) {
    return false;
  }
  if (!tableType->elemType.isFunc() || !tableType->elemType.isNullable()) {
    return d.fail("bad table element type");
  }
  if (!DecodeLimits(d, codeMeta, LimitsKind::Table, &tableType->limits)) {
    return false;
  }

  // If there's a maximum, check it is in range.  The check to exclude
  // initial > maximum is carried out by the DecodeLimits call above, so
  // we don't repeat it here.
  if (tableType->limits.initial >
          MaxTableElemsValidation(tableType->limits.addressType) ||
      ((tableType->limits.maximum.isSome() &&
        tableType->limits.maximum.value() >
            MaxTableElemsValidation(tableType->limits.addressType)))) {
    return d.fail("too many table elements");
  }

  return true;
}

static bool DecodeGlobalType(Decoder& d, const CodeMetadata* codeMeta,
                             GlobalType* globalType) {
  if (!d.readValType(*codeMeta->types, codeMeta->features(),
                     &globalType->type)) {
    return d.fail("expected global type");
  }
  if (!globalType->type.isNumber()) {
    return d.fail("bad global type");
  }

  uint8_t flags;
  if (!d.readFixedU8(&flags)) {
    return d.fail("expected global flags");
  }

  if (flags & ~uint8_t(GlobalTypeImmediate::AllowedMask)) {
    return d.fail("unexpected bits set in global flags");
  }

  globalType->isMutable = flags & uint8_t(GlobalTypeImmediate::IsMutable);
  return true;
}

static bool DecodeMemoryType(Decoder& d, const CodeMetadata* codeMeta,
                             Limits* limits) {
  if (!DecodeLimits(d, codeMeta, LimitsKind::Memory, limits)) {
    return false;
  }

  uint64_t maxField =
      MaxMemoryPagesValidation(limits->addressType, limits->pageSize);

  if (limits->initial > maxField) {
    return d.fail("initial memory size too big");
  }

  if (limits->maximum && *limits->maximum > maxField) {
    return d.fail("maximum memory size too big");
  }

  if (limits->shared == Shareable::True &&
      codeMeta->sharedMemoryEnabled() == Shareable::False) {
    return d.fail("shared memory is disabled");
  }

  return true;
}


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
static bool DecodeImportType(Decoder& d, DefinitionKind importKind,
                             const CodeMetadata* codeMeta,
                             const ModuleMetadata* moduleMeta,
                             ExternType* importType) {
  switch (importKind) {
    case DefinitionKind::Function: {
      uint32_t funcTypeIndex;
      if (!DecodeFuncTypeIndex(d, codeMeta->types, &funcTypeIndex)) {
        return false;
      }
      *importType = ExternType::func(funcTypeIndex);
      break;
    }
    case DefinitionKind::Table: {
      TableType tableType;
      if (!DecodeTableType(d, codeMeta, &tableType)) {
        return false;
      }
      *importType = ExternType::table(tableType);
      break;
    }
    case DefinitionKind::Memory: {
      Limits memType;
      if (!DecodeMemoryType(d, codeMeta, &memType)) {
        return false;
      }
      *importType = ExternType::memory(memType);
      break;
    }
    case DefinitionKind::Global: {
      GlobalType globalType;
      if (!DecodeGlobalType(d, codeMeta, &globalType)) {
        return false;
      }
      *importType = ExternType::global(globalType);
      break;
    }
    default:
      return d.fail("unsupported import kind");
  }

  return true;
}

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

static bool DecodeImportGroup(Decoder& d, CodeMetadata* codeMeta,
                              ModuleMetadata* moduleMeta) {
  CacheableName moduleName;
  if (!DecodeName(d, &moduleName)) {
    return d.fail("expected valid import module name");
  }
  CacheableName itemName;
  if (!DecodeName(d, &itemName)) {
    return d.fail("expected valid import name");
  }
  uint8_t rawImportKind;
  if (!d.readFixedU8(&rawImportKind)) {
    return d.fail("failed to read import kind");
  }

  // Single-item encoding
  mozilla::CheckedUint32 numImportsSoFar(moduleMeta->imports.length());
  numImportsSoFar += 1;
  if (!numImportsSoFar.isValid() || numImportsSoFar.value() > MaxImports) {
    return d.fail("too many imports");
  }
  ExternType importType;
  if (!DecodeImportType(d, DefinitionKind(rawImportKind), codeMeta, moduleMeta,
                        &importType)) {
    return false;
  }
  return AddImport(d, moduleName, itemName, importType, codeMeta, moduleMeta);
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

static bool DecodeImportSection(Decoder& d, CodeMetadata* codeMeta,
                                ModuleMetadata* moduleMeta) {
  MaybeBytecodeRange range;
  if (!d.startSection(SectionId::Import, codeMeta, &range, "import")) {
    return false;
  }
  if (!range) {
    return true;
  }

  uint32_t numImportGroups;
  if (!d.readVarU32(&numImportGroups)) {
    return d.fail("failed to read number of imports");
  }
  for (uint32_t i = 0; i < numImportGroups; i++) {
    if (!DecodeImportGroup(d, codeMeta, moduleMeta)) {
      return false;
    }
  }

  if (!d.finishSection(*range, "import")) {
    return false;
  }

  codeMeta->numFuncImports = codeMeta->funcs.length();
  if (!codeMeta->knownFuncImports.resize(codeMeta->numFuncImports)) {
    return false;
  }
  codeMeta->numGlobalImports = codeMeta->globals.length();
  return true;
}

static bool DecodeFunctionSection(Decoder& d, CodeMetadata* codeMeta) {
  MaybeBytecodeRange range;
  if (!d.startSection(SectionId::Function, codeMeta, &range, "function")) {
    return false;
  }
  if (!range) {
    return true;
  }

  uint32_t numDefs;
  if (!d.readVarU32(&numDefs)) {
    return d.fail("expected number of function definitions");
  }

  CheckedInt<uint32_t> numFuncs = codeMeta->funcs.length();
  numFuncs += numDefs;
  if (!numFuncs.isValid() || numFuncs.value() > MaxFuncs) {
    return d.fail("too many functions");
  }

  if (!codeMeta->funcs.reserve(numFuncs.value())) {
    return false;
  }

  for (uint32_t i = 0; i < numDefs; i++) {
    uint32_t funcTypeIndex;
    if (!DecodeFuncTypeIndex(d, codeMeta->types, &funcTypeIndex)) {
      return false;
    }
    codeMeta->funcs.infallibleAppend(funcTypeIndex);
  }

  return d.finishSection(*range, "function");
}

static bool DecodeTableSection(Decoder& d, CodeMetadata* codeMeta) {
  MaybeBytecodeRange range;
  if (!d.startSection(SectionId::Table, codeMeta, &range, "table")) {
    return false;
  }
  if (!range) {
    return true;
  }

  uint32_t numDefs;
  if (!d.readVarU32(&numDefs)) {
    return d.fail("failed to read number of tables");
  }

  CheckedInt<uint32_t> numTables = codeMeta->tables.length();
  numTables += numDefs;
  if (!numTables.isValid() || numTables.value() > MaxTables) {
    return d.fail("too many tables");
  }

  if (!codeMeta->tables.reserve(numTables.value())) {
    return false;
  }

  for (uint32_t i = 0; i < numDefs; ++i) {
    TableType tableType;
    if (!DecodeTableType(d, codeMeta, &tableType)) {
      return false;
    }
    codeMeta->tables.infallibleAppend(TableDesc(
        tableType, mozilla::Nothing(), /*isImported=*/false));
  }

  return d.finishSection(*range, "table");
}

static bool DecodeMemorySection(Decoder& d, CodeMetadata* codeMeta) {
  MaybeBytecodeRange range;
  if (!d.startSection(SectionId::Memory, codeMeta, &range, "memory")) {
    return false;
  }
  if (!range) {
    return true;
  }

  uint32_t numDefs;
  if (!d.readVarU32(&numDefs)) {
    return d.fail("failed to read number of memories");
  }

  CheckedInt<uint32_t> numMemories = codeMeta->memories.length();
  numMemories += numDefs;
  if (!numMemories.isValid() || numMemories.value() > MaxMemories) {
    return d.fail("too many memories");
  }

  if (!codeMeta->memories.reserve(numMemories.value())) {
    return false;
  }

  for (uint32_t i = 0; i < numDefs; ++i) {
    Limits limits;
    if (!DecodeMemoryType(d, codeMeta, &limits)) {
      return false;
    }
    codeMeta->memories.infallibleAppend(MemoryDesc(limits));
  }

  return d.finishSection(*range, "memory");
}

static bool DecodeGlobalSection(Decoder& d, CodeMetadata* codeMeta) {
  MaybeBytecodeRange range;
  if (!d.startSection(SectionId::Global, codeMeta, &range, "global")) {
    return false;
  }
  if (!range) {
    return true;
  }

  uint32_t numDefs;
  if (!d.readVarU32(&numDefs)) {
    return d.fail("expected number of globals");
  }

  CheckedInt<uint32_t> numGlobals = codeMeta->globals.length();
  numGlobals += numDefs;
  if (!numGlobals.isValid() || numGlobals.value() > MaxGlobals) {
    return d.fail("too many globals");
  }

  if (!codeMeta->globals.reserve(numGlobals.value())) {
    return false;
  }

  for (uint32_t i = 0; i < numDefs; i++) {
    GlobalType type;
    if (!DecodeGlobalType(d, codeMeta, &type)) {
      return false;
    }

    InitExpr initializer;
    if (!InitExpr::decodeAndValidate(d, codeMeta, type.type, &initializer)) {
      return false;
    }

    codeMeta->globals.infallibleAppend(
        GlobalDesc(std::move(initializer), type.isMutable));
  }

  return d.finishSection(*range, "global");
}

using NameSet = HashSet<Span<char>, NameHasher, SystemAllocPolicy>;

[[nodiscard]] static bool DecodeExportName(Decoder& d, NameSet* dupSet,
                                           CacheableName* exportName) {
  if (!DecodeName(d, exportName)) {
    d.fail("expected valid export name");
    return false;
  }

  NameSet::AddPtr p = dupSet->lookupForAdd(exportName->utf8Bytes());
  if (p) {
    d.fail("duplicate export");
    return false;
  }

  return dupSet->add(p, exportName->utf8Bytes());
}

static bool DecodeExport(Decoder& d, CodeMetadata* codeMeta,
                         ModuleMetadata* moduleMeta, NameSet* dupSet) {
  CacheableName fieldName;
  if (!DecodeExportName(d, dupSet, &fieldName)) {
    return false;
  }

  uint8_t exportKind;
  if (!d.readFixedU8(&exportKind)) {
    return d.fail("failed to read export kind");
  }

  switch (DefinitionKind(exportKind)) {
    case DefinitionKind::Function: {
      uint32_t funcIndex;
      if (!d.readVarU32(&funcIndex)) {
        return d.fail("expected function index");
      }

      if (funcIndex >= codeMeta->numFuncs()) {
        return d.fail("exported function index out of bounds");
      }

      codeMeta->funcs[funcIndex].declareFuncExported(/* eager */ true,
                                                     /* canRefFunc */ true);
      return moduleMeta->exports.emplaceBack(std::move(fieldName), funcIndex,
                                             DefinitionKind::Function);
    }
    case DefinitionKind::Table: {
      uint32_t tableIndex;
      if (!d.readVarU32(&tableIndex)) {
        return d.fail("expected table index");
      }

      if (tableIndex >= codeMeta->tables.length()) {
        return d.fail("exported table index out of bounds");
      }
      codeMeta->tables[tableIndex].isExported = true;
      return moduleMeta->exports.emplaceBack(std::move(fieldName), tableIndex,
                                             DefinitionKind::Table);
    }
    case DefinitionKind::Memory: {
      uint32_t memoryIndex;
      if (!d.readVarU32(&memoryIndex)) {
        return d.fail("expected memory index");
      }

      if (memoryIndex >= codeMeta->numMemories()) {
        return d.fail("exported memory index out of bounds");
      }

      return moduleMeta->exports.emplaceBack(std::move(fieldName), memoryIndex,
                                             DefinitionKind::Memory);
    }
    case DefinitionKind::Global: {
      uint32_t globalIndex;
      if (!d.readVarU32(&globalIndex)) {
        return d.fail("expected global index");
      }

      if (globalIndex >= codeMeta->globals.length()) {
        return d.fail("exported global index out of bounds");
      }

      GlobalDesc* global = &codeMeta->globals[globalIndex];
      global->setIsExport();

      return moduleMeta->exports.emplaceBack(std::move(fieldName), globalIndex,
                                             DefinitionKind::Global);
    }
    case DefinitionKind::Tag: {
      uint32_t tagIndex;
      if (!d.readVarU32(&tagIndex)) {
        return d.fail("expected tag index");
      }
      if (tagIndex >= codeMeta->tags.length()) {
        return d.fail("exported tag index out of bounds");
      }

      codeMeta->tags[tagIndex].isExport = true;
      return moduleMeta->exports.emplaceBack(std::move(fieldName), tagIndex,
                                             DefinitionKind::Tag);
    }
    default:
      return d.fail("unexpected export kind");
  }

  MOZ_CRASH("unreachable");
}

static bool DecodeExportSection(Decoder& d, CodeMetadata* codeMeta,
                                ModuleMetadata* moduleMeta) {
  MaybeBytecodeRange range;
  if (!d.startSection(SectionId::Export, codeMeta, &range, "export")) {
    return false;
  }
  if (!range) {
    return true;
  }

  NameSet dupSet;

  uint32_t numExports;
  if (!d.readVarU32(&numExports)) {
    return d.fail("failed to read number of exports");
  }

  if (numExports > MaxExports) {
    return d.fail("too many exports");
  }

  for (uint32_t i = 0; i < numExports; i++) {
    if (!DecodeExport(d, codeMeta, moduleMeta, &dupSet)) {
      return false;
    }
  }

  return d.finishSection(*range, "export");
}

static bool DecodeStartSection(Decoder& d, CodeMetadata* codeMeta,
                               ModuleMetadata* moduleMeta) {
  MaybeBytecodeRange range;
  if (!d.startSection(SectionId::Start, codeMeta, &range, "start")) {
    return false;
  }
  if (!range) {
    return true;
  }

  uint32_t funcIndex;
  if (!d.readVarU32(&funcIndex)) {
    return d.fail("failed to read start func index");
  }

  if (funcIndex >= codeMeta->numFuncs()) {
    return d.fail("unknown start function");
  }

  const FuncType& funcType = codeMeta->getFuncType(funcIndex);
  if (funcType.results().length() > 0) {
    return d.fail("start function must not return anything");
  }

  if (funcType.args().length()) {
    return d.fail("start function must be nullary");
  }

  codeMeta->funcs[funcIndex].declareFuncExported(/* eager */ true,
                                                 /* canFuncRef */ false);
  codeMeta->startFuncIndex = Some(funcIndex);

  return d.finishSection(*range, "start");
}

// https://webassembly.github.io/spec/core/binary/modules.html#element-section
//
// wasm 1.0 element segments are always active against table 0 and always
// contain a plain vector of function indices. Encoders (including our own
// text-to-binary compiler) still emit the post-1.0 flags-prefixed encoding
// for this content -- e.g. an explicit literal table index in the text
// format selects the "active with table index" flag value (2) even when
// that index is 0 -- so the flags byte must still be decoded, just
// restricted to the two flag values with no passive/declared/expression
// content.
static bool DecodeElemSegment(Decoder& d, CodeMetadata* codeMeta,
                              ModuleMetadata* moduleMeta) {
  uint32_t flags;
  if (!d.readVarU32(&flags)) {
    return d.fail("expected elem segment flags field");
  }
  if (flags != 0 && flags != 2) {
    return d.fail("invalid elem segment flags field");
  }

  if (codeMeta->tables.length() == 0) {
    return d.fail("elem segment requires a table");
  }

  uint32_t tableIndex = 0;
  if (flags == 2 && !d.readVarU32(&tableIndex)) {
    return d.fail("expected table index");
  }
  if (tableIndex != 0) {
    return d.fail("table index out of range for element segment");
  }

  ModuleElemSegment seg = ModuleElemSegment();
  seg.kind = ModuleElemSegment::Kind::Active;
  seg.tableIndex = 0;

  InitExpr offset;
  if (!InitExpr::decodeAndValidate(
          d, codeMeta, ToValType(codeMeta->tables[0].addressType()),
          &offset)) {
    return false;
  }
  seg.offsetIfActive.emplace(std::move(offset));
  seg.elemType = RefType::func().asNonNullable();

  if (flags == 2) {
    uint8_t elemKind;
    if (!d.readFixedU8(&elemKind)) {
      return d.fail("expected element kind");
    }
    if (elemKind != uint8_t(DefinitionKind::Function)) {
      return d.fail("invalid element kind");
    }
  }

  uint32_t numElems;
  if (!d.readVarU32(&numElems)) {
    return d.fail("expected element segment size");
  }

  if (numElems > MaxElemSegmentLength) {
    return d.fail("too many elements in element segment");
  }

  seg.encoding = ModuleElemSegment::Encoding::Indices;
  if (!seg.elemIndices.reserve(numElems)) {
    return false;
  }

  for (uint32_t i = 0; i < numElems; i++) {
    uint32_t elemIndex;
    if (!d.readVarU32(&elemIndex)) {
      return d.fail("failed to read element index");
    }
    if (elemIndex >= codeMeta->numFuncs()) {
      return d.fail("element index out of range");
    }

    seg.elemIndices.infallibleAppend(elemIndex);
    codeMeta->funcs[elemIndex].declareFuncExported(/*eager=*/false,
                                                   /*canRefFunc=*/true);
  }

  codeMeta->elemSegmentTypes.infallibleAppend(seg.elemType);
  moduleMeta->elemSegments.infallibleAppend(std::move(seg));

  return true;
}

static bool DecodeElemSection(Decoder& d, CodeMetadata* codeMeta,
                              ModuleMetadata* moduleMeta) {
  MaybeBytecodeRange range;
  if (!d.startSection(SectionId::Elem, codeMeta, &range, "elem")) {
    return false;
  }
  if (!range) {
    return true;
  }

  uint32_t numSegments;
  if (!d.readVarU32(&numSegments)) {
    return d.fail("failed to read number of elem segments");
  }

  if (numSegments > MaxElemSegments) {
    return d.fail("too many elem segments");
  }

  if (!moduleMeta->elemSegments.reserve(numSegments) ||
      !codeMeta->elemSegmentTypes.reserve(numSegments)) {
    return false;
  }

  for (uint32_t i = 0; i < numSegments; i++) {
    if (!DecodeElemSegment(d, codeMeta, moduleMeta)) {
      return false;
    }
  }

  return d.finishSection(*range, "elem");
}

bool wasm::StartsCodeSection(const uint8_t* begin, const uint8_t* end,
                             BytecodeRange* codeSection) {
  UniqueChars unused;
  Decoder d(begin, end, 0, &unused);

  if (!DecodePreamble(d, EncodingVersionModule)) {
    return false;
  }

  while (!d.done()) {
    uint8_t id;
    BytecodeRange range;
    if (!d.readSectionHeader(&id, &range)) {
      return false;
    }

    if (id == uint8_t(SectionId::Code)) {
      if (range.size() > MaxCodeSectionBytes) {
        return false;
      }

      *codeSection = range;
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

bool wasm::DecodeModuleEnvironment(Decoder& d, CodeMetadata* codeMeta,
                                   ModuleMetadata* moduleMeta) {
  if (!DecodePreamble(d, EncodingVersionModule)) {
    return false;
  }

  if (!DecodeTypeSection(d, codeMeta)) {
    return false;
  }

  if (!DecodeImportSection(d, codeMeta, moduleMeta)) {
    return false;
  }

  // Eagerly check imports for future link errors against any known builtin
  // module.
  if (!CheckImportsAgainstBuiltinModules(d, codeMeta, moduleMeta)) {
    return false;
  }

  if (!DecodeFunctionSection(d, codeMeta)) {
    return false;
  }

  if (!DecodeTableSection(d, codeMeta)) {
    return false;
  }

  if (!DecodeMemorySection(d, codeMeta)) {
    return false;
  }

  if (!CheckBuiltinImportsHaveMemory(d, codeMeta)) {
    return false;
  }

  if (!DecodeGlobalSection(d, codeMeta)) {
    return false;
  }

  if (!DecodeExportSection(d, codeMeta, moduleMeta)) {
    return false;
  }

  if (!DecodeStartSection(d, codeMeta, moduleMeta)) {
    return false;
  }

  if (!DecodeElemSection(d, codeMeta, moduleMeta)) {
    return false;
  }

  if (!d.startSection(SectionId::Code, codeMeta, &codeMeta->codeSectionRange,
                      "code")) {
    return false;
  }

  if (codeMeta->codeSectionRange &&
      codeMeta->codeSectionRange->size() > MaxCodeSectionBytes) {
    return d.fail("code section too big");
  }

  return true;
}

static bool DecodeFunctionBody(Decoder& d, const CodeMetadata& codeMeta,
                               uint32_t funcIndex) {
  uint32_t bodySize;
  if (!d.readVarU32(&bodySize)) {
    return d.fail("expected number of function body bytes");
  }

  if (bodySize > MaxFunctionBytes) {
    return d.fail("function body too big");
  }

  if (d.bytesRemain() < bodySize) {
    return d.fail("function body length too big");
  }

  return ValidateFunctionBody(codeMeta, funcIndex, bodySize, d);
}

static bool DecodeCodeSection(Decoder& d, CodeMetadata* codeMeta) {
  if (!codeMeta->codeSectionRange) {
    if (codeMeta->numFuncDefs() != 0) {
      return d.fail("expected code section");
    }
    return true;
  }

  uint32_t numFuncDefs;
  if (!d.readVarU32(&numFuncDefs)) {
    return d.fail("expected function body count");
  }

  if (numFuncDefs != codeMeta->numFuncDefs()) {
    return d.fail(
        "function body count does not match function signature count");
  }

  for (uint32_t funcDefIndex = 0; funcDefIndex < numFuncDefs; funcDefIndex++) {
    if (!DecodeFunctionBody(d, *codeMeta,
                            codeMeta->numFuncImports + funcDefIndex)) {
      return false;
    }
  }

  return d.finishSection(*codeMeta->codeSectionRange, "code");
}

// https://webassembly.github.io/spec/core/binary/modules.html#data-section
//
// wasm 1.0 data segments are always active against memory 0. As with elem
// segments (see DecodeElemSegment), encoders may still choose the post-1.0
// flags-prefixed encoding (kind 2) for this content, so the kind byte must
// still be decoded, just restricted to the two kinds with no passive
// content.
static bool DecodeDataSection(Decoder& d, CodeMetadata* codeMeta,
                              ModuleMetadata* moduleMeta) {
  MaybeBytecodeRange range;
  if (!d.startSection(SectionId::Data, codeMeta, &range, "data")) {
    return false;
  }
  if (!range) {
    return true;
  }

  uint32_t numSegments;
  if (!d.readVarU32(&numSegments)) {
    return d.fail("failed to read number of data segments");
  }

  if (numSegments > MaxDataSegments) {
    return d.fail("too many data segments");
  }

  for (uint32_t i = 0; i < numSegments; i++) {
    uint32_t kind;
    if (!d.readVarU32(&kind)) {
      return d.fail("expected data initializer-kind field");
    }
    if (kind != 0 && kind != 2) {
      return d.fail("invalid data initializer-kind field");
    }

    if (codeMeta->numMemories() == 0) {
      return d.fail("data segment requires a memory section");
    }

    uint32_t memoryIndex = 0;
    if (kind == 2 && !d.readVarU32(&memoryIndex)) {
      return d.fail("expected memory index");
    }
    if (memoryIndex != 0) {
      return d.fail("invalid memory index");
    }

    DataSegmentRange segRange;
    segRange.memoryIndex = 0;

    InitExpr segOffset;
    ValType exprType = ToValType(codeMeta->memories[0].addressType());
    if (!InitExpr::decodeAndValidate(d, codeMeta, exprType, &segOffset)) {
      return false;
    }
    segRange.offsetIfActive.emplace(std::move(segOffset));

    if (!d.readVarU32(&segRange.length)) {
      return d.fail("expected segment size");
    }

    if (segRange.length > MaxDataSegmentLengthPages * StandardPageSizeBytes) {
      return d.fail("segment size too big");
    }

    segRange.bytecodeOffset = d.currentOffset();

    if (!d.readBytes(segRange.length)) {
      return d.fail("data segment shorter than declared");
    }

    if (!moduleMeta->dataSegmentRanges.append(std::move(segRange))) {
      return false;
    }
  }

  return d.finishSection(*range, "data");
}

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

bool wasm::DecodeModuleTail(Decoder& d, CodeMetadata* codeMeta,
                            ModuleMetadata* moduleMeta) {
  if (!DecodeDataSection(d, codeMeta, moduleMeta)) {
    return false;
  }

  if (!DecodeNameSection(d, codeMeta, moduleMeta)) {
    return false;
  }

  while (!d.done()) {
    if (!d.skipCustomSection(codeMeta)) {
      return false;
    }
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

[[nodiscard]] static bool ValidateModule(JSContext* cx,
                                         const BytecodeSource& bytecode,
                                         const FeatureArgs& features,
                                         const FeatureOptions& options,
                                         UniqueChars* error) {
  SharedCompileArgs compileArgs = CompileArgs::buildForValidation(features);
  if (!compileArgs) {
    return false;
  }
  MutableModuleMetadata moduleMeta = js_new<ModuleMetadata>();
  if (!moduleMeta || !moduleMeta->init(*compileArgs)) {
    return false;
  }
  MutableCodeMetadata codeMeta = moduleMeta->codeMeta;

  Decoder envDecoder(bytecode.envSpan(), bytecode.envRange().start, error);
  if (!DecodeModuleEnvironment(envDecoder, codeMeta, moduleMeta)) {
    return false;
  }

  if (bytecode.hasCodeSection()) {
    // DecodeModuleEnvironment will stop and return true if there is an unknown
    // section before the code section. We must check this and return an error.
    if (!moduleMeta->codeMeta->codeSectionRange) {
      envDecoder.fail("unknown section before code section");
      return false;
    }

    // Our pre-parse that split the module should ensure that after we've
    // parsed the environment there are no bytes left.
    MOZ_RELEASE_ASSERT(envDecoder.done());

    Decoder codeDecoder(bytecode.codeSpan(), bytecode.codeRange().start, error);
    if (!DecodeCodeSection(codeDecoder, codeMeta)) {
      return false;
    }
    // Our pre-parse that split the module should ensure that after we've
    // parsed the code section there are no bytes left.
    MOZ_RELEASE_ASSERT(codeDecoder.done());

    Decoder tailDecoder(bytecode.tailSpan(), bytecode.tailRange().start, error);
    if (!DecodeModuleTail(tailDecoder, codeMeta, moduleMeta)) {
      return false;
    }
    // Decoding the module tail should consume all remaining bytes.
    MOZ_RELEASE_ASSERT(tailDecoder.done());
  } else {
    if (!DecodeCodeSection(envDecoder, codeMeta)) {
      return false;
    }
    if (!DecodeModuleTail(envDecoder, codeMeta, moduleMeta)) {
      return false;
    }
    // Decoding the module tail should consume all remaining bytes.
    MOZ_RELEASE_ASSERT(envDecoder.done());
  }

  MOZ_ASSERT(!*error, "unreported error in decoding");
  return true;
}

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
  FeatureArgs features = FeatureArgs::build(cx, options);

#ifdef ENABLE_WASM_COMPONENTS
  if (features.components) {
    Decoder preambleDecoder(bytecode.envSpan(), bytecode.envRange().start,
                            error);
    if (IsComponent(preambleDecoder)) {
      return ValidateComponent(cx, bytecode, options, error);
    }
  }
#endif
  return ValidateModule(cx, bytecode, features, options, error);
}
