/* The C ABI of itasca, a formally verified WebAssembly 1.0 validator.
 *
 * Hand-maintained. Keep it in step with `itasca_glue`'s `src/`: a new entry
 * point is a declaration here, and a new hook is a slot in the right struct
 * plus a row in the matching VW_FOR_EACH_*_HOOK list. The static_asserts
 * below catch a hook added on one side and not the other.
 *
 * The enum values are SpiderMonkey's, which are the binary format's, so handing
 * one of these across is an identity and not a conversion.
 *
 * C++ rather than C, since the consumer this exists for is SpiderMonkey.
 *
 * Opcode hooks may reject validation but cannot override its checks. The
 * experimental stream API additionally requires coherent byte publication and
 * exclusive ownership of batch handles; see VwStreamVisitor below. Its complete
 * Aeneas model compiles, but its correctness proofs are still deferred.
 */

#ifndef itasca_h
#define itasca_h

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

extern "C" {

/* Handles. Opaque: read them through the accessors below.
 *
 * VwState and VwContext are borrowed for the duration of a hook call and must
 * not be retained. The rest are owned, and each has a matching free.
 */
struct VwState;
struct VwContext;
struct VwEnv;
struct VwTail;
struct VwBatch;

/* TypeCode's value types, which are the bytes valtype encodes as. Handing one
 * of these to SpiderMonkey is an identity and not a conversion.
 */
enum class VwValType : uint8_t {
  I32 = 0x7f,
  I64 = 0x7e,
  F32 = 0x7d,
  F64 = 0x7c,
};

/* The type of an operand stack entry: a value type, or bottom in unreachable
 * code. Bot is its own case so it cannot be mistaken for a value type.
 */
enum class VwStackType : uint8_t {
  I32 = 0x7f,
  I64 = 0x7e,
  F32 = 0x7d,
  F64 = 0x7c,
  /* TypeCode::Limit, which is what StackType::bottom() packs. */
  Bot = 0x80,
};

/* A block's type, as the byte blocktype encodes as. Wasm 1.0 blocks take no
 * parameters and return at most one value, so the whole of it fits in one tag.
 * SpiderMonkey's BlockType is a tagged pointer and cannot be matched, so this is
 * the format's encoding instead.
 */
enum class VwBlockType : uint8_t {
  Empty = 0x40,
  I32 = 0x7f,
  I64 = 0x7e,
  F32 = 0x7d,
  F64 = 0x7c,
};

/* LabelKind, in SpiderMonkey's order. What an `end` closed: a bare `if` closes
 * as Then, so a consumer that needs the elided empty `else` knows to supply it.
 */
enum class VwLabelKind : uint8_t {
  Body = 0,
  Block = 1,
  Loop = 2,
  Then = 3,
  Else = 4,
};

/* GlobalTypeImmediate::IsMutable, which is the format's mutability byte. */
enum class VwMut : uint8_t {
  Const = 0,
  Var = 1,
};

/* DefinitionKind, which is the format's import and export kind byte. */
enum class VwExternKind : uint8_t {
  Func = 0x00,
  Table = 0x01,
  Memory = 0x02,
  Global = 0x03,
};

/* Which instruction the constant expression is, as its own opcode. Wasm 1.0
 * allows exactly one instruction there.
 */
enum class VwConstExprKind : uint8_t {
  GlobalGet = 0x23,
  I32 = 0x41,
  I64 = 0x42,
  F32 = 0x43,
  F64 = 0x44,
};

struct VwMemArg {
  uint32_t align;
  uint32_t offset;
};

struct VwLimits {
  uint32_t min;
  uint32_t max;
  bool has_max;
};

/* Wasm 1.0 has one element type, funcref, so it is not carried. */
struct VwTableType {
  VwLimits limits;
};

struct VwMemType {
  VwLimits limits;
};

struct VwGlobalType {
  VwValType valtype;
  VwMut mutability;
};

/* A constant expression. Wasm 1.0 allows exactly one instruction, so this is
 * the decoded value and not a byte range.
 */
struct VwConstExpr {
  VwConstExprKind kind;
  /* I32 and I64: the value, sign extended. F32 and F64: the bit pattern, since
   * the verified core has no floating-point arithmetic. GlobalGet: the index.
   */
  uint64_t bits;
};

/* What an entry point reports. error is VW_OK or a VW_ERR_*; op_error is a
 * VW_OP_ERR_* when error is VW_ERR_READ or VW_ERR_BODY and zero otherwise;
 * detail is the index or byte the error carries, or zero.
 */
struct VwStatus {
  int32_t error;
  int32_t op_error;
  uint32_t detail;
};

/* What every hook receives first. */
struct VwHookCtx {
  void* user;
  /* Byte offset of this operator within the bytes handed to
   * itasca_validate_code_entry, which is the same frame as its `pos` and its
   * `*out_error_offset`. A consumer that was handed a region rather than the
   * whole module adds the region's own offset to get a module offset.
   *
   * Derived rather than read: a hook sees the cursor positioned after the
   * operator's immediates, and the cursor one hook sees is the start of the
   * operator the next hook sees. The body's base comes from the validator, by
   * way of on_function_start.
   */
  size_t op_offset;
};

/* The load and store hooks, one per opcode, each with a checked memarg. */
struct VwMemoryHooks {
  uint32_t (*on_i32_load)(const VwHookCtx*, const VwState*, VwMemArg);
  uint32_t (*on_i64_load)(const VwHookCtx*, const VwState*, VwMemArg);
  uint32_t (*on_f32_load)(const VwHookCtx*, const VwState*, VwMemArg);
  uint32_t (*on_f64_load)(const VwHookCtx*, const VwState*, VwMemArg);
  uint32_t (*on_i32_load8_s)(const VwHookCtx*, const VwState*, VwMemArg);
  uint32_t (*on_i32_load8_u)(const VwHookCtx*, const VwState*, VwMemArg);
  uint32_t (*on_i32_load16_s)(const VwHookCtx*, const VwState*, VwMemArg);
  uint32_t (*on_i32_load16_u)(const VwHookCtx*, const VwState*, VwMemArg);
  uint32_t (*on_i64_load8_s)(const VwHookCtx*, const VwState*, VwMemArg);
  uint32_t (*on_i64_load8_u)(const VwHookCtx*, const VwState*, VwMemArg);
  uint32_t (*on_i64_load16_s)(const VwHookCtx*, const VwState*, VwMemArg);
  uint32_t (*on_i64_load16_u)(const VwHookCtx*, const VwState*, VwMemArg);
  uint32_t (*on_i64_load32_s)(const VwHookCtx*, const VwState*, VwMemArg);
  uint32_t (*on_i64_load32_u)(const VwHookCtx*, const VwState*, VwMemArg);
  uint32_t (*on_i32_store)(const VwHookCtx*, const VwState*, VwMemArg);
  uint32_t (*on_i64_store)(const VwHookCtx*, const VwState*, VwMemArg);
  uint32_t (*on_f32_store)(const VwHookCtx*, const VwState*, VwMemArg);
  uint32_t (*on_f64_store)(const VwHookCtx*, const VwState*, VwMemArg);
  uint32_t (*on_i32_store8)(const VwHookCtx*, const VwState*, VwMemArg);
  uint32_t (*on_i32_store16)(const VwHookCtx*, const VwState*, VwMemArg);
  uint32_t (*on_i64_store8)(const VwHookCtx*, const VwState*, VwMemArg);
  uint32_t (*on_i64_store16)(const VwHookCtx*, const VwState*, VwMemArg);
  uint32_t (*on_i64_store32)(const VwHookCtx*, const VwState*, VwMemArg);
};

/* The numeric hooks, one per opcode. No immediates. */
struct VwNumericHooks {
  uint32_t (*on_i32_eqz)(const VwHookCtx*, const VwState*);
  uint32_t (*on_i32_eq)(const VwHookCtx*, const VwState*);
  uint32_t (*on_i32_ne)(const VwHookCtx*, const VwState*);
  uint32_t (*on_i32_lt_s)(const VwHookCtx*, const VwState*);
  uint32_t (*on_i32_lt_u)(const VwHookCtx*, const VwState*);
  uint32_t (*on_i32_gt_s)(const VwHookCtx*, const VwState*);
  uint32_t (*on_i32_gt_u)(const VwHookCtx*, const VwState*);
  uint32_t (*on_i32_le_s)(const VwHookCtx*, const VwState*);
  uint32_t (*on_i32_le_u)(const VwHookCtx*, const VwState*);
  uint32_t (*on_i32_ge_s)(const VwHookCtx*, const VwState*);
  uint32_t (*on_i32_ge_u)(const VwHookCtx*, const VwState*);
  uint32_t (*on_i64_eqz)(const VwHookCtx*, const VwState*);
  uint32_t (*on_i64_eq)(const VwHookCtx*, const VwState*);
  uint32_t (*on_i64_ne)(const VwHookCtx*, const VwState*);
  uint32_t (*on_i64_lt_s)(const VwHookCtx*, const VwState*);
  uint32_t (*on_i64_lt_u)(const VwHookCtx*, const VwState*);
  uint32_t (*on_i64_gt_s)(const VwHookCtx*, const VwState*);
  uint32_t (*on_i64_gt_u)(const VwHookCtx*, const VwState*);
  uint32_t (*on_i64_le_s)(const VwHookCtx*, const VwState*);
  uint32_t (*on_i64_le_u)(const VwHookCtx*, const VwState*);
  uint32_t (*on_i64_ge_s)(const VwHookCtx*, const VwState*);
  uint32_t (*on_i64_ge_u)(const VwHookCtx*, const VwState*);
  uint32_t (*on_f32_eq)(const VwHookCtx*, const VwState*);
  uint32_t (*on_f32_ne)(const VwHookCtx*, const VwState*);
  uint32_t (*on_f32_lt)(const VwHookCtx*, const VwState*);
  uint32_t (*on_f32_gt)(const VwHookCtx*, const VwState*);
  uint32_t (*on_f32_le)(const VwHookCtx*, const VwState*);
  uint32_t (*on_f32_ge)(const VwHookCtx*, const VwState*);
  uint32_t (*on_f64_eq)(const VwHookCtx*, const VwState*);
  uint32_t (*on_f64_ne)(const VwHookCtx*, const VwState*);
  uint32_t (*on_f64_lt)(const VwHookCtx*, const VwState*);
  uint32_t (*on_f64_gt)(const VwHookCtx*, const VwState*);
  uint32_t (*on_f64_le)(const VwHookCtx*, const VwState*);
  uint32_t (*on_f64_ge)(const VwHookCtx*, const VwState*);
  uint32_t (*on_i32_sub)(const VwHookCtx*, const VwState*);
  uint32_t (*on_i32_mul)(const VwHookCtx*, const VwState*);
  uint32_t (*on_i32_div_s)(const VwHookCtx*, const VwState*);
  uint32_t (*on_i32_div_u)(const VwHookCtx*, const VwState*);
  uint32_t (*on_i32_rem_s)(const VwHookCtx*, const VwState*);
  uint32_t (*on_i32_rem_u)(const VwHookCtx*, const VwState*);
  uint32_t (*on_i32_and)(const VwHookCtx*, const VwState*);
  uint32_t (*on_i32_or)(const VwHookCtx*, const VwState*);
  uint32_t (*on_i32_xor)(const VwHookCtx*, const VwState*);
  uint32_t (*on_i32_shl)(const VwHookCtx*, const VwState*);
  uint32_t (*on_i32_shr_s)(const VwHookCtx*, const VwState*);
  uint32_t (*on_i32_shr_u)(const VwHookCtx*, const VwState*);
  uint32_t (*on_i32_rotl)(const VwHookCtx*, const VwState*);
  uint32_t (*on_i32_rotr)(const VwHookCtx*, const VwState*);
  uint32_t (*on_i64_add)(const VwHookCtx*, const VwState*);
  uint32_t (*on_i64_sub)(const VwHookCtx*, const VwState*);
  uint32_t (*on_i64_mul)(const VwHookCtx*, const VwState*);
  uint32_t (*on_i64_div_s)(const VwHookCtx*, const VwState*);
  uint32_t (*on_i64_div_u)(const VwHookCtx*, const VwState*);
  uint32_t (*on_i64_rem_s)(const VwHookCtx*, const VwState*);
  uint32_t (*on_i64_rem_u)(const VwHookCtx*, const VwState*);
  uint32_t (*on_i64_and)(const VwHookCtx*, const VwState*);
  uint32_t (*on_i64_or)(const VwHookCtx*, const VwState*);
  uint32_t (*on_i64_xor)(const VwHookCtx*, const VwState*);
  uint32_t (*on_i64_shl)(const VwHookCtx*, const VwState*);
  uint32_t (*on_i64_shr_s)(const VwHookCtx*, const VwState*);
  uint32_t (*on_i64_shr_u)(const VwHookCtx*, const VwState*);
  uint32_t (*on_i64_rotl)(const VwHookCtx*, const VwState*);
  uint32_t (*on_i64_rotr)(const VwHookCtx*, const VwState*);
  uint32_t (*on_f32_add)(const VwHookCtx*, const VwState*);
  uint32_t (*on_f32_sub)(const VwHookCtx*, const VwState*);
  uint32_t (*on_f32_mul)(const VwHookCtx*, const VwState*);
  uint32_t (*on_f32_div)(const VwHookCtx*, const VwState*);
  uint32_t (*on_f32_min)(const VwHookCtx*, const VwState*);
  uint32_t (*on_f32_max)(const VwHookCtx*, const VwState*);
  uint32_t (*on_f32_copysign)(const VwHookCtx*, const VwState*);
  uint32_t (*on_f64_add)(const VwHookCtx*, const VwState*);
  uint32_t (*on_f64_sub)(const VwHookCtx*, const VwState*);
  uint32_t (*on_f64_mul)(const VwHookCtx*, const VwState*);
  uint32_t (*on_f64_div)(const VwHookCtx*, const VwState*);
  uint32_t (*on_f64_min)(const VwHookCtx*, const VwState*);
  uint32_t (*on_f64_max)(const VwHookCtx*, const VwState*);
  uint32_t (*on_f64_copysign)(const VwHookCtx*, const VwState*);
  uint32_t (*on_i32_clz)(const VwHookCtx*, const VwState*);
  uint32_t (*on_i32_ctz)(const VwHookCtx*, const VwState*);
  uint32_t (*on_i32_popcnt)(const VwHookCtx*, const VwState*);
  uint32_t (*on_i32_add)(const VwHookCtx*, const VwState*);
  uint32_t (*on_i64_clz)(const VwHookCtx*, const VwState*);
  uint32_t (*on_i64_ctz)(const VwHookCtx*, const VwState*);
  uint32_t (*on_i64_popcnt)(const VwHookCtx*, const VwState*);
  uint32_t (*on_f32_abs)(const VwHookCtx*, const VwState*);
  uint32_t (*on_f32_neg)(const VwHookCtx*, const VwState*);
  uint32_t (*on_f32_ceil)(const VwHookCtx*, const VwState*);
  uint32_t (*on_f32_floor)(const VwHookCtx*, const VwState*);
  uint32_t (*on_f32_trunc)(const VwHookCtx*, const VwState*);
  uint32_t (*on_f32_nearest)(const VwHookCtx*, const VwState*);
  uint32_t (*on_f32_sqrt)(const VwHookCtx*, const VwState*);
  uint32_t (*on_f64_abs)(const VwHookCtx*, const VwState*);
  uint32_t (*on_f64_neg)(const VwHookCtx*, const VwState*);
  uint32_t (*on_f64_ceil)(const VwHookCtx*, const VwState*);
  uint32_t (*on_f64_floor)(const VwHookCtx*, const VwState*);
  uint32_t (*on_f64_trunc)(const VwHookCtx*, const VwState*);
  uint32_t (*on_f64_nearest)(const VwHookCtx*, const VwState*);
  uint32_t (*on_f64_sqrt)(const VwHookCtx*, const VwState*);
  uint32_t (*on_i32_wrap_i64)(const VwHookCtx*, const VwState*);
  uint32_t (*on_i32_trunc_f32_s)(const VwHookCtx*, const VwState*);
  uint32_t (*on_i32_trunc_f32_u)(const VwHookCtx*, const VwState*);
  uint32_t (*on_i32_trunc_f64_s)(const VwHookCtx*, const VwState*);
  uint32_t (*on_i32_trunc_f64_u)(const VwHookCtx*, const VwState*);
  uint32_t (*on_i64_extend_i32_s)(const VwHookCtx*, const VwState*);
  uint32_t (*on_i64_extend_i32_u)(const VwHookCtx*, const VwState*);
  uint32_t (*on_i64_trunc_f32_s)(const VwHookCtx*, const VwState*);
  uint32_t (*on_i64_trunc_f32_u)(const VwHookCtx*, const VwState*);
  uint32_t (*on_i64_trunc_f64_s)(const VwHookCtx*, const VwState*);
  uint32_t (*on_i64_trunc_f64_u)(const VwHookCtx*, const VwState*);
  uint32_t (*on_f32_convert_i32_s)(const VwHookCtx*, const VwState*);
  uint32_t (*on_f32_convert_i32_u)(const VwHookCtx*, const VwState*);
  uint32_t (*on_f32_convert_i64_s)(const VwHookCtx*, const VwState*);
  uint32_t (*on_f32_convert_i64_u)(const VwHookCtx*, const VwState*);
  uint32_t (*on_f32_demote_f64)(const VwHookCtx*, const VwState*);
  uint32_t (*on_f64_convert_i32_s)(const VwHookCtx*, const VwState*);
  uint32_t (*on_f64_convert_i32_u)(const VwHookCtx*, const VwState*);
  uint32_t (*on_f64_convert_i64_s)(const VwHookCtx*, const VwState*);
  uint32_t (*on_f64_convert_i64_u)(const VwHookCtx*, const VwState*);
  uint32_t (*on_f64_promote_f32)(const VwHookCtx*, const VwState*);
  uint32_t (*on_i32_reinterpret_f32)(const VwHookCtx*, const VwState*);
  uint32_t (*on_i64_reinterpret_f64)(const VwHookCtx*, const VwState*);
  uint32_t (*on_f32_reinterpret_i32)(const VwHookCtx*, const VwState*);
  uint32_t (*on_f64_reinterpret_i64)(const VwHookCtx*, const VwState*);
};

/* The hooks. Zero-initialise and fill in only the ones you want: a null slot is
 * skipped, so an unused hook costs nothing.
 *
 * A hook is called once the operator has been decoded and validated, with its
 * immediates already checked. The state it is handed is the state *after* the
 * operator's effect, so its stacks and the innermost frame's polymorphic_base
 * are what say what the operand types are and whether the code is reachable.
 */
struct VwVisitor {
  uint32_t (*on_function_start)(const VwHookCtx*, const VwContext*, uint32_t, size_t, size_t);
  uint32_t (*on_unreachable)(const VwHookCtx*, const VwState*);
  uint32_t (*on_nop)(const VwHookCtx*, const VwState*);
  uint32_t (*on_block)(const VwHookCtx*, const VwState*, VwBlockType);
  uint32_t (*on_loop)(const VwHookCtx*, const VwState*, VwBlockType);
  uint32_t (*on_if)(const VwHookCtx*, const VwState*, VwBlockType);
  uint32_t (*on_else)(const VwHookCtx*, const VwState*, VwBlockType);
  uint32_t (*on_end)(const VwHookCtx*, const VwState*, VwLabelKind, VwBlockType);
  uint32_t (*on_br)(const VwHookCtx*, const VwState*, uint32_t, VwBlockType);
  uint32_t (*on_br_if)(const VwHookCtx*, const VwState*, uint32_t, VwBlockType);
  uint32_t (*on_br_table_label)(const VwHookCtx*, const VwState*, uint32_t);
  uint32_t (*on_br_table)(const VwHookCtx*, const VwState*, uint32_t, VwBlockType);
  uint32_t (*on_return)(const VwHookCtx*, const VwState*);
  uint32_t (*on_call)(const VwHookCtx*, const VwState*, uint32_t);
  uint32_t (*on_call_indirect)(const VwHookCtx*, const VwState*, uint32_t);
  uint32_t (*on_drop)(const VwHookCtx*, const VwState*, VwStackType);
  uint32_t (*on_select)(const VwHookCtx*, const VwState*, VwStackType);
  uint32_t (*on_local_get)(const VwHookCtx*, const VwState*, uint32_t);
  uint32_t (*on_local_set)(const VwHookCtx*, const VwState*, uint32_t);
  uint32_t (*on_local_tee)(const VwHookCtx*, const VwState*, uint32_t);
  uint32_t (*on_global_get)(const VwHookCtx*, const VwState*, uint32_t);
  uint32_t (*on_global_set)(const VwHookCtx*, const VwState*, uint32_t);
  uint32_t (*on_memory_size)(const VwHookCtx*, const VwState*);
  uint32_t (*on_memory_grow)(const VwHookCtx*, const VwState*);
  uint32_t (*on_i32_const)(const VwHookCtx*, const VwState*, int32_t);
  uint32_t (*on_i64_const)(const VwHookCtx*, const VwState*, int64_t);
  uint32_t (*on_f32_const)(const VwHookCtx*, const VwState*, uint32_t);
  uint32_t (*on_f64_const)(const VwHookCtx*, const VwState*, uint64_t);

  VwMemoryHooks memory;
  VwNumericHooks numeric;
};

/* One pointer per hook. A mismatch is a hook declared on one side of the
 * boundary and not the other; `itasca_glue`'s
 * `tests::the_vtable_has_one_slot_per_hook` is the same check from the Rust
 * side.
 */
static_assert(sizeof(VwMemoryHooks) == 23 * sizeof(void*), "memory hooks");
static_assert(sizeof(VwNumericHooks) == 123 * sizeof(void*), "numeric hooks");
static_assert(sizeof(VwVisitor) == 174 * sizeof(void*), "hooks");

/* What a hook returns. */
static const uint32_t VW_VISIT_OK = 0;
static const uint32_t VW_VISIT_OOM = 1;

/* VwStatus::error. */
static const int32_t VW_OK = 0;
static const int32_t VW_ERR_READ = 1;
static const int32_t VW_ERR_INVALID_MAGIC = 2;
static const int32_t VW_ERR_INVALID_VERSION = 3;
static const int32_t VW_ERR_UNEXPECTED_EOF = 4;
static const int32_t VW_ERR_INVALID_TYPE = 5;
static const int32_t VW_ERR_UNKNOWN_SECTION = 6;
static const int32_t VW_ERR_SECTION_OUT_OF_ORDER = 7;
static const int32_t VW_ERR_SECTION_SIZE_MISMATCH = 8;
static const int32_t VW_ERR_FUNC_CODE_MISMATCH = 9;
static const int32_t VW_ERR_INVALID_UTF8 = 10;
static const int32_t VW_ERR_UNKNOWN_TYPE = 11;
static const int32_t VW_ERR_UNKNOWN_FUNC = 12;
static const int32_t VW_ERR_UNKNOWN_TABLE = 13;
static const int32_t VW_ERR_UNKNOWN_MEMORY = 14;
static const int32_t VW_ERR_UNKNOWN_GLOBAL = 15;
static const int32_t VW_ERR_DUPLICATE_EXPORT_NAME = 16;
static const int32_t VW_ERR_LIMITS_OUT_OF_RANGE = 17;
static const int32_t VW_ERR_MULTIPLE_MEMORIES = 18;
static const int32_t VW_ERR_MULTIPLE_TABLES = 19;
static const int32_t VW_ERR_INVALID_START_TYPE = 20;
static const int32_t VW_ERR_MUTABLE_GLOBAL_IN_CONST_EXPR = 21;
static const int32_t VW_ERR_INVALID_CONST_EXPR = 22;
static const int32_t VW_ERR_TOO_MANY_RESULTS = 23;
static const int32_t VW_ERR_TOO_MANY_LOCALS = 24;
static const int32_t VW_ERR_MODULE_TOO_LARGE = 25;
static const int32_t VW_ERR_BODY = 26;
static const int32_t VW_ERR_MISSING_CODE_SECTION = 28;
static const int32_t VW_ERR_VISITOR = 29;

/* VwStatus::op_error. */
static const int32_t VW_OP_ERR_UNEXPECTED_EOF = 1;
static const int32_t VW_OP_ERR_LEB_TOO_LONG = 2;
static const int32_t VW_OP_ERR_UNRECOGNIZED_OPCODE = 3;
static const int32_t VW_OP_ERR_INVALID_BLOCK_TYPE = 4;
static const int32_t VW_OP_ERR_TYPE_MISMATCH = 5;
static const int32_t VW_OP_ERR_EMPTY_STACK = 6;
static const int32_t VW_OP_ERR_STACK_MISMATCH = 7;
static const int32_t VW_OP_ERR_UNKNOWN_LABEL = 8;
static const int32_t VW_OP_ERR_UNKNOWN_LOCAL = 9;
static const int32_t VW_OP_ERR_UNKNOWN_GLOBAL = 10;
static const int32_t VW_OP_ERR_IMMUTABLE_GLOBAL = 11;
static const int32_t VW_OP_ERR_UNKNOWN_MEMORY = 12;
static const int32_t VW_OP_ERR_UNKNOWN_FUNC = 13;
static const int32_t VW_OP_ERR_UNKNOWN_TYPE = 14;
static const int32_t VW_OP_ERR_UNKNOWN_TABLE = 15;
static const int32_t VW_OP_ERR_TOO_MANY_RESULTS = 16;
static const int32_t VW_OP_ERR_RESERVED_BYTE_NOT_ZERO = 17;
static const int32_t VW_OP_ERR_INVALID_ALIGNMENT = 18;
static const int32_t VW_OP_ERR_TRAILING_BYTES = 19;
static const int32_t VW_OP_ERR_BODY_TOO_LARGE = 20;
static const int32_t VW_OP_ERR_VISITOR_OOM = 21;

/* The caps the validator enforces, which are also the caps
 * the completeness theorems carry as hypotheses.
 */
static const uint64_t VW_MAX_MEMORY_PAGES = 65536;
static const uint64_t VW_MAX_TABLES = 1;
static const uint64_t VW_MAX_MEMORIES = 1;
static const uint64_t VW_MAX_RESULTS = 1;
static const uint64_t VW_MAX_FUNCTION_BYTES = 7654321;
static const uint64_t VW_MAX_MODULE_BYTES = 1073741824;
static const uint64_t VW_MAX_LOCALS = 50000;

/* Entry points. */
VwValType itasca_context_local(const VwContext* c, size_t i);
size_t itasca_context_num_locals(const VwContext* c);
VwConstExpr itasca_env_defined_global_init(const VwEnv* e, size_t i);
VwGlobalType itasca_env_defined_global_type(const VwEnv* e, size_t i);
uint32_t itasca_env_element_func(const VwEnv* e, size_t i, size_t j);
size_t itasca_env_element_num_funcs(const VwEnv* e, size_t i);
VwConstExpr itasca_env_element_offset(const VwEnv* e, size_t i);
uint32_t itasca_env_element_table_index(const VwEnv* e, size_t i);
uint32_t itasca_env_export_index(const VwEnv* e, size_t i);
VwExternKind itasca_env_export_kind(const VwEnv* e, size_t i);
const uint8_t* itasca_env_export_name(const VwEnv* e, size_t i, size_t* out_len);
void itasca_env_free(VwEnv* env);
uint32_t itasca_env_func_def_type_index(const VwEnv* e, size_t i);
bool itasca_env_has_start(const VwEnv* e);
uint32_t itasca_env_import_func_type_index(const VwEnv* e, size_t i);
VwGlobalType itasca_env_import_global(const VwEnv* e, size_t i);
VwExternKind itasca_env_import_kind(const VwEnv* e, size_t i);
VwMemType itasca_env_import_memory(const VwEnv* e, size_t i);
const uint8_t* itasca_env_import_module(const VwEnv* e, size_t i, size_t* out_len);
const uint8_t* itasca_env_import_name(const VwEnv* e, size_t i, size_t* out_len);
VwTableType itasca_env_import_table(const VwEnv* e, size_t i);
VwMemType itasca_env_memory(const VwEnv* e, size_t i);
size_t itasca_env_num_defined_globals(const VwEnv* e);
size_t itasca_env_num_elements(const VwEnv* e);
size_t itasca_env_num_exports(const VwEnv* e);
size_t itasca_env_num_func_defs(const VwEnv* e);
size_t itasca_env_num_funcs(const VwEnv* e);
size_t itasca_env_num_globals(const VwEnv* e);
size_t itasca_env_num_imports(const VwEnv* e);
size_t itasca_env_num_memories(const VwEnv* e);
size_t itasca_env_num_tables(const VwEnv* e);
size_t itasca_env_num_types(const VwEnv* e);
uint32_t itasca_env_start(const VwEnv* e);
VwTableType itasca_env_table(const VwEnv* e, size_t i);
size_t itasca_env_type_num_params(const VwEnv* e, size_t type_idx);
size_t itasca_env_type_num_results(const VwEnv* e, size_t type_idx);
VwValType itasca_env_type_param(const VwEnv* e, size_t type_idx, size_t i);
VwValType itasca_env_type_result(const VwEnv* e, size_t type_idx, size_t i);
const char* itasca_error_str(int32_t code);
const char* itasca_op_error_str(int32_t code);
const uint8_t* itasca_tail_data_bytes(const VwTail* t, size_t i, size_t* out_len);
uint32_t itasca_tail_data_memory_index(const VwTail* t, size_t i);
VwConstExpr itasca_tail_data_offset(const VwTail* t, size_t i);
void itasca_tail_free(VwTail* tail);
size_t itasca_tail_num_data(const VwTail* t);
VwStatus itasca_validate_code_entry(const uint8_t* data, size_t len, size_t pos, const VwEnv* env, size_t index, const VwVisitor* visitor, void* user, size_t* out_next_pos, size_t* out_error_offset);
/* Same as itasca_validate_code_entry, driven with baseline's hooks. Baseline's
 * hooks are fixed extern "C" symbols (see WasmBaselineCompile.cpp) rather than a
 * VwVisitor a caller builds and hands over, so there is no visitor argument: the
 * Rust side (itasca_glue's BaselineVisitor) calls them directly by name, which
 * is what lets them be inlined across the FFI boundary instead of dispatched
 * through a vtable.
 */
VwStatus itasca_validate_code_entry_baseline(const uint8_t* data, size_t len, size_t pos, const VwEnv* env, size_t index, void* user, size_t* out_next_pos, size_t* out_error_offset);

enum class VwStreamRegion : uint32_t { Environment, Code, Tail };

struct VwStreamConfig {
  size_t batch_bytes;
  size_t max_batches;
};
struct VwBatchEntry {
  size_t index, start, contents, end;
  const uint8_t* data;
};
// All callbacks are required. Ranges publish initialized immutable bytes.
// The environment is borrowed until finish; tail and submit transfer
// ownership even on failure. complete transfers a live batch back; the driver
// checks its module and batch IDs. finish must join and release all remaining
// jobs before returning, including on error. Successful validation transfers
// the environment through out_env; failure leaves out_env null.
struct VwStreamVisitor {
  uint32_t (*range)(void*, VwStreamRegion, size_t, size_t, const uint8_t**,
                    size_t*);
  uint32_t (*environment)(void*, const VwEnv*, size_t, size_t, VwStreamConfig*);
  uint32_t (*custom)(void*, VwStreamRegion, size_t, size_t, size_t);
  uint32_t (*submit)(void*, VwBatch*);
  uint32_t (*complete)(void*, VwBatch**);
  uint32_t (*code_end)(void*);
  uint32_t (*tail)(void*, VwTail*);
  void (*finish)(void*, bool success);
};
VwStatus itasca_validate_stream(const VwStreamVisitor*, void*, size_t code_len,
                                VwEnv** out_env);
void itasca_batch_free(VwBatch*);
size_t itasca_batch_len(const VwBatch*);
bool itasca_batch_is_last(const VwBatch*);
VwStatus itasca_batch_next_entry(VwBatch*, VwBatchEntry*);
VwStatus itasca_batch_validate_func(VwBatch*, size_t, const VwVisitor*, void*,
                                    size_t*);
VwStatus itasca_batch_validate_func_baseline(VwBatch*, size_t, void*, size_t*);

}  // extern "C"

#endif  /* itasca_h */
