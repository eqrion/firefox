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

#ifndef wasm_itasca_ops_h
#define wasm_itasca_ops_h

#include "wasm/itasca.h"

// The memory and numeric hooks' opcode, one row per hook, shared between
// baseline and Ion.
//
// Both compilers install one thunk per hook and dispatch straight to an
// emitter named after the opcode; this table is the only place that says
// which opcode a hook is. F(hook, Name) means the hook is `on_##hook`,
// declared in itasca.h's VW_FOR_EACH_{MEMORY,NUMERIC}_HOOK, and the
// opcode is `Op::Name`. Keep the row order matching itasca.h's hook lists,
// which is also the order `VW_FOR_EACH_{MEMORY,NUMERIC}_HOOK` visits them in.

#define SM_VW_FOR_EACH_MEMORY_OP(F) \
  F(on_i32_load, I32Load) \
  F(on_i64_load, I64Load) \
  F(on_f32_load, F32Load) \
  F(on_f64_load, F64Load) \
  F(on_i32_load8_s, I32Load8S) \
  F(on_i32_load8_u, I32Load8U) \
  F(on_i32_load16_s, I32Load16S) \
  F(on_i32_load16_u, I32Load16U) \
  F(on_i64_load8_s, I64Load8S) \
  F(on_i64_load8_u, I64Load8U) \
  F(on_i64_load16_s, I64Load16S) \
  F(on_i64_load16_u, I64Load16U) \
  F(on_i64_load32_s, I64Load32S) \
  F(on_i64_load32_u, I64Load32U) \
  F(on_i32_store, I32Store) \
  F(on_i64_store, I64Store) \
  F(on_f32_store, F32Store) \
  F(on_f64_store, F64Store) \
  F(on_i32_store8, I32Store8) \
  F(on_i32_store16, I32Store16) \
  F(on_i64_store8, I64Store8) \
  F(on_i64_store16, I64Store16) \
  F(on_i64_store32, I64Store32)

#define SM_VW_FOR_EACH_NUMERIC_OP(F) \
  F(on_i32_eqz, I32Eqz) \
  F(on_i32_eq, I32Eq) \
  F(on_i32_ne, I32Ne) \
  F(on_i32_lt_s, I32LtS) \
  F(on_i32_lt_u, I32LtU) \
  F(on_i32_gt_s, I32GtS) \
  F(on_i32_gt_u, I32GtU) \
  F(on_i32_le_s, I32LeS) \
  F(on_i32_le_u, I32LeU) \
  F(on_i32_ge_s, I32GeS) \
  F(on_i32_ge_u, I32GeU) \
  F(on_i64_eqz, I64Eqz) \
  F(on_i64_eq, I64Eq) \
  F(on_i64_ne, I64Ne) \
  F(on_i64_lt_s, I64LtS) \
  F(on_i64_lt_u, I64LtU) \
  F(on_i64_gt_s, I64GtS) \
  F(on_i64_gt_u, I64GtU) \
  F(on_i64_le_s, I64LeS) \
  F(on_i64_le_u, I64LeU) \
  F(on_i64_ge_s, I64GeS) \
  F(on_i64_ge_u, I64GeU) \
  F(on_f32_eq, F32Eq) \
  F(on_f32_ne, F32Ne) \
  F(on_f32_lt, F32Lt) \
  F(on_f32_gt, F32Gt) \
  F(on_f32_le, F32Le) \
  F(on_f32_ge, F32Ge) \
  F(on_f64_eq, F64Eq) \
  F(on_f64_ne, F64Ne) \
  F(on_f64_lt, F64Lt) \
  F(on_f64_gt, F64Gt) \
  F(on_f64_le, F64Le) \
  F(on_f64_ge, F64Ge) \
  F(on_i32_sub, I32Sub) \
  F(on_i32_mul, I32Mul) \
  F(on_i32_div_s, I32DivS) \
  F(on_i32_div_u, I32DivU) \
  F(on_i32_rem_s, I32RemS) \
  F(on_i32_rem_u, I32RemU) \
  F(on_i32_and, I32And) \
  F(on_i32_or, I32Or) \
  F(on_i32_xor, I32Xor) \
  F(on_i32_shl, I32Shl) \
  F(on_i32_shr_s, I32ShrS) \
  F(on_i32_shr_u, I32ShrU) \
  F(on_i32_rotl, I32Rotl) \
  F(on_i32_rotr, I32Rotr) \
  F(on_i64_add, I64Add) \
  F(on_i64_sub, I64Sub) \
  F(on_i64_mul, I64Mul) \
  F(on_i64_div_s, I64DivS) \
  F(on_i64_div_u, I64DivU) \
  F(on_i64_rem_s, I64RemS) \
  F(on_i64_rem_u, I64RemU) \
  F(on_i64_and, I64And) \
  F(on_i64_or, I64Or) \
  F(on_i64_xor, I64Xor) \
  F(on_i64_shl, I64Shl) \
  F(on_i64_shr_s, I64ShrS) \
  F(on_i64_shr_u, I64ShrU) \
  F(on_i64_rotl, I64Rotl) \
  F(on_i64_rotr, I64Rotr) \
  F(on_f32_add, F32Add) \
  F(on_f32_sub, F32Sub) \
  F(on_f32_mul, F32Mul) \
  F(on_f32_div, F32Div) \
  F(on_f32_min, F32Min) \
  F(on_f32_max, F32Max) \
  F(on_f32_copysign, F32CopySign) \
  F(on_f64_add, F64Add) \
  F(on_f64_sub, F64Sub) \
  F(on_f64_mul, F64Mul) \
  F(on_f64_div, F64Div) \
  F(on_f64_min, F64Min) \
  F(on_f64_max, F64Max) \
  F(on_f64_copysign, F64CopySign) \
  F(on_i32_clz, I32Clz) \
  F(on_i32_ctz, I32Ctz) \
  F(on_i32_popcnt, I32Popcnt) \
  F(on_i32_add, I32Add) \
  F(on_i64_clz, I64Clz) \
  F(on_i64_ctz, I64Ctz) \
  F(on_i64_popcnt, I64Popcnt) \
  F(on_f32_abs, F32Abs) \
  F(on_f32_neg, F32Neg) \
  F(on_f32_ceil, F32Ceil) \
  F(on_f32_floor, F32Floor) \
  F(on_f32_trunc, F32Trunc) \
  F(on_f32_nearest, F32Nearest) \
  F(on_f32_sqrt, F32Sqrt) \
  F(on_f64_abs, F64Abs) \
  F(on_f64_neg, F64Neg) \
  F(on_f64_ceil, F64Ceil) \
  F(on_f64_floor, F64Floor) \
  F(on_f64_trunc, F64Trunc) \
  F(on_f64_nearest, F64Nearest) \
  F(on_f64_sqrt, F64Sqrt) \
  F(on_i32_wrap_i64, I32WrapI64) \
  F(on_i32_trunc_f32_s, I32TruncF32S) \
  F(on_i32_trunc_f32_u, I32TruncF32U) \
  F(on_i32_trunc_f64_s, I32TruncF64S) \
  F(on_i32_trunc_f64_u, I32TruncF64U) \
  F(on_i64_extend_i32_s, I64ExtendI32S) \
  F(on_i64_extend_i32_u, I64ExtendI32U) \
  F(on_i64_trunc_f32_s, I64TruncF32S) \
  F(on_i64_trunc_f32_u, I64TruncF32U) \
  F(on_i64_trunc_f64_s, I64TruncF64S) \
  F(on_i64_trunc_f64_u, I64TruncF64U) \
  F(on_f32_convert_i32_s, F32ConvertI32S) \
  F(on_f32_convert_i32_u, F32ConvertI32U) \
  F(on_f32_convert_i64_s, F32ConvertI64S) \
  F(on_f32_convert_i64_u, F32ConvertI64U) \
  F(on_f32_demote_f64, F32DemoteF64) \
  F(on_f64_convert_i32_s, F64ConvertI32S) \
  F(on_f64_convert_i32_u, F64ConvertI32U) \
  F(on_f64_convert_i64_s, F64ConvertI64S) \
  F(on_f64_convert_i64_u, F64ConvertI64U) \
  F(on_f64_promote_f32, F64PromoteF32) \
  F(on_i32_reinterpret_f32, I32ReinterpretF32) \
  F(on_i64_reinterpret_f64, I64ReinterpretF64) \
  F(on_f32_reinterpret_i32, F32ReinterpretI32) \
  F(on_f64_reinterpret_i64, F64ReinterpretI64)

// A row per hook, on this side too.
//
// itasca.h's own static_asserts pin its structs to one pointer per hook, but
// nothing there can see these lists, and a list short by a row compiles
// perfectly well: the hook it left out stays null, itasca skips a null hook
// by design, and the compiler never sees that operator. That is a miscompile
// rather than a rejection, so it is worth a build error.
#define SM_VW_COUNT_OP(hook, Name) +1

static_assert(0 SM_VW_FOR_EACH_MEMORY_OP(SM_VW_COUNT_OP) ==
                  sizeof(VwMemoryHooks) / sizeof(void*),
              "SM_VW_FOR_EACH_MEMORY_OP has drifted from VwMemoryHooks");
static_assert(0 SM_VW_FOR_EACH_NUMERIC_OP(SM_VW_COUNT_OP) ==
                  sizeof(VwNumericHooks) / sizeof(void*),
              "SM_VW_FOR_EACH_NUMERIC_OP has drifted from VwNumericHooks");

#undef SM_VW_COUNT_OP

#endif  // wasm_itasca_ops_h
