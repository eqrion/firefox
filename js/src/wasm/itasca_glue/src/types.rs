//! The `repr(C)` mirrors of itasca's public types, and the flat status codes
//! the entry points report.
//!
//! Nothing verified carries a `repr` attribute, and adding one to suit a C
//! consumer would be letting the interface reach into the proofs, so every type
//! that crosses the boundary by value has a mirror here. The handles go the
//! other way: C++ holds opaque handles and accesses their contents through Rust.
//! Streaming callbacks borrow the environment; successful validation transfers ownership.

use itasca::code::{BlockType, Context, LabelKind, MemArg, OpIterState, StackType};
use itasca::error::{Error, OpError, VisitError};
use itasca::module::{Env, Tail};
use itasca::types::{ConstExpr, GlobalType, Limits, MemType, Mut, TableType, ValueType};

// --- Handles ---

/// The running validator state, as a hook sees it. Borrowed for the duration of
/// the call and never owned by the consumer.
#[repr(transparent)]
pub struct VwState(pub(crate) OpIterState);

/// One function's locals and results. Borrowed, as `VwState` is.
#[repr(transparent)]
pub struct VwContext(pub(crate) Context);

/// Everything the binary declares before the code section. Owned by the
/// consumer after successful module validation, freed with `itasca_env_free`.
/// The streaming environment callback only borrows it until scope exit.
#[repr(transparent)]
pub struct VwEnv(pub(crate) Env);

/// Everything after the code section. Owned, freed with `itasca_tail_free`.
#[repr(transparent)]
pub struct VwTail(pub(crate) Tail);

// --- Value and instruction types ---
//
// The discriminants are SpiderMonkey's, which are the binary format's, so
// handing one of these across is an identity and not a conversion. Where
// SpiderMonkey's own type cannot be matched -- its `BlockType` is a tagged
// pointer -- the format's encoding is used instead, which is the same idea.

/// `TypeCode`'s value types, which are the bytes `valtype` encodes as.
#[repr(u8)]
#[derive(Clone, Copy)]
pub enum VwValType {
    I32 = 0x7f,
    I64 = 0x7e,
    F32 = 0x7d,
    F64 = 0x7c,
}

/// The type of an operand stack entry: a value type, or bottom in unreachable
/// code. `Bot` is `TypeCode::Limit`, which is what SpiderMonkey's
/// `StackType::bottom()` packs, so this is its `StackType` for the cases Wasm
/// 1.0 has.
#[repr(u8)]
#[derive(Clone, Copy)]
pub enum VwStackType {
    I32 = 0x7f,
    I64 = 0x7e,
    F32 = 0x7d,
    F64 = 0x7c,
    Bot = 0x80,
}

/// A block's type, as the byte `blocktype` encodes as. Wasm 1.0 blocks take no
/// parameters and return at most one value, so the whole of it fits in one tag.
///
/// SpiderMonkey's `BlockType` is a tagged pointer and cannot be matched, so this
/// is the format's encoding: `0x40` for the empty type, the value type's own
/// byte otherwise.
#[repr(u8)]
#[derive(Clone, Copy)]
pub enum VwBlockType {
    Empty = 0x40,
    I32 = 0x7f,
    I64 = 0x7e,
    F32 = 0x7d,
    F64 = 0x7c,
}

/// `LabelKind`, in SpiderMonkey's order. What an `end` closed: a bare `if`
/// closes as `Then`, so a consumer that needs the elided empty `else` knows to
/// supply it.
#[repr(u8)]
#[derive(Clone, Copy)]
pub enum VwLabelKind {
    Body = 0,
    Block = 1,
    Loop = 2,
    Then = 3,
    Else = 4,
}

/// `GlobalTypeImmediate::IsMutable`, which is the format's mutability byte.
#[repr(u8)]
#[derive(Clone, Copy)]
pub enum VwMut {
    Const = 0,
    Var = 1,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct VwMemArg {
    pub align: u32,
    pub offset: u32,
}

// --- Module types ---

#[repr(C)]
#[derive(Clone, Copy)]
pub struct VwLimits {
    pub min: u32,
    pub max: u32,
    pub has_max: bool,
}

/// A table type. Wasm 1.0 has one element type, `funcref`, so the element type
/// is not carried.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct VwTableType {
    pub limits: VwLimits,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct VwMemType {
    pub limits: VwLimits,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct VwGlobalType {
    pub valtype: VwValType,
    pub mutability: VwMut,
}

/// `DefinitionKind`, which is the format's import and export kind byte.
#[repr(u8)]
#[derive(Clone, Copy)]
pub enum VwExternKind {
    Func = 0x00,
    Table = 0x01,
    Memory = 0x02,
    Global = 0x03,
}

/// Which instruction the constant expression is, as its own opcode. Wasm 1.0
/// allows exactly one instruction there, so the opcode is the whole tag.
#[repr(u8)]
#[derive(Clone, Copy)]
pub enum VwConstExprKind {
    GlobalGet = 0x23,
    I32 = 0x41,
    I64 = 0x42,
    F32 = 0x43,
    F64 = 0x44,
}

/// A constant expression. Wasm 1.0 allows exactly one instruction, so this is
/// the decoded value rather than a byte range.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct VwConstExpr {
    pub kind: VwConstExprKind,
    /// `I32` and `I64`: the value, sign extended. `F32` and `F64`: the bit
    /// pattern, since the verified core has no floating-point arithmetic.
    /// `GlobalGet`: the global index.
    pub bits: u64,
}

// --- What a hook returns ---

/// Carry on. The only answer that lets validation finish.
pub const VW_VISIT_OK: u32 = 0;
/// Decline the body, reporting out of memory. Any non-zero value is read this
/// way; the constant exists so a consumer has something to name.
pub const VW_VISIT_OOM: u32 = 1;

// --- What an entry point returns ---

pub const VW_OK: i32 = 0;

pub const VW_ERR_READ: i32 = 1;
pub const VW_ERR_INVALID_MAGIC: i32 = 2;
pub const VW_ERR_INVALID_VERSION: i32 = 3;
pub const VW_ERR_UNEXPECTED_EOF: i32 = 4;
pub const VW_ERR_INVALID_TYPE: i32 = 5;
pub const VW_ERR_UNKNOWN_SECTION: i32 = 6;
pub const VW_ERR_SECTION_OUT_OF_ORDER: i32 = 7;
pub const VW_ERR_SECTION_SIZE_MISMATCH: i32 = 8;
pub const VW_ERR_FUNC_CODE_MISMATCH: i32 = 9;
pub const VW_ERR_INVALID_UTF8: i32 = 10;
pub const VW_ERR_UNKNOWN_TYPE: i32 = 11;
pub const VW_ERR_UNKNOWN_FUNC: i32 = 12;
pub const VW_ERR_UNKNOWN_TABLE: i32 = 13;
pub const VW_ERR_UNKNOWN_MEMORY: i32 = 14;
pub const VW_ERR_UNKNOWN_GLOBAL: i32 = 15;
pub const VW_ERR_DUPLICATE_EXPORT_NAME: i32 = 16;
pub const VW_ERR_LIMITS_OUT_OF_RANGE: i32 = 17;
pub const VW_ERR_MULTIPLE_MEMORIES: i32 = 18;
pub const VW_ERR_MULTIPLE_TABLES: i32 = 19;
pub const VW_ERR_INVALID_START_TYPE: i32 = 20;
pub const VW_ERR_MUTABLE_GLOBAL_IN_CONST_EXPR: i32 = 21;
pub const VW_ERR_INVALID_CONST_EXPR: i32 = 22;
pub const VW_ERR_TOO_MANY_RESULTS: i32 = 23;
pub const VW_ERR_TOO_MANY_LOCALS: i32 = 24;
pub const VW_ERR_MODULE_TOO_LARGE: i32 = 25;
pub const VW_ERR_BODY: i32 = 26;
/// Appended rather than slotted in beside `VW_ERR_FUNC_CODE_MISMATCH`, so that
/// no code a consumer has already compiled against changes value. 27 was a
/// bad-argument code, from when the interface reported a caller's mistake
/// instead of aborting on it.
pub const VW_ERR_MISSING_CODE_SECTION: i32 = 28;
/// Appended for the same reason. `op_error` carries which decline it was, the
/// way `VW_ERR_BODY` does, so a consumer that declines for more than one reason
/// still has one code to read.
pub const VW_ERR_VISITOR: i32 = 29;

pub const VW_OP_ERR_UNEXPECTED_EOF: i32 = 1;
pub const VW_OP_ERR_LEB_TOO_LONG: i32 = 2;
pub const VW_OP_ERR_UNRECOGNIZED_OPCODE: i32 = 3;
pub const VW_OP_ERR_INVALID_BLOCK_TYPE: i32 = 4;
pub const VW_OP_ERR_TYPE_MISMATCH: i32 = 5;
pub const VW_OP_ERR_EMPTY_STACK: i32 = 6;
pub const VW_OP_ERR_STACK_MISMATCH: i32 = 7;
pub const VW_OP_ERR_UNKNOWN_LABEL: i32 = 8;
pub const VW_OP_ERR_UNKNOWN_LOCAL: i32 = 9;
pub const VW_OP_ERR_UNKNOWN_GLOBAL: i32 = 10;
pub const VW_OP_ERR_IMMUTABLE_GLOBAL: i32 = 11;
pub const VW_OP_ERR_UNKNOWN_MEMORY: i32 = 12;
pub const VW_OP_ERR_UNKNOWN_FUNC: i32 = 13;
pub const VW_OP_ERR_UNKNOWN_TYPE: i32 = 14;
pub const VW_OP_ERR_UNKNOWN_TABLE: i32 = 15;
pub const VW_OP_ERR_TOO_MANY_RESULTS: i32 = 16;
pub const VW_OP_ERR_RESERVED_BYTE_NOT_ZERO: i32 = 17;
pub const VW_OP_ERR_INVALID_ALIGNMENT: i32 = 18;
pub const VW_OP_ERR_TRAILING_BYTES: i32 = 19;
pub const VW_OP_ERR_BODY_TOO_LARGE: i32 = 20;
/// The consumer declined the body. Not a validity verdict: the bytes may well
/// encode a well-typed function, and this says only that validation did not
/// finish.
pub const VW_OP_ERR_VISITOR_OOM: i32 = 21;

/// What an entry point reports. `error` is `VW_OK` or one of the `VW_ERR_*`
/// codes; `op_error` is a `VW_OP_ERR_*` code when `error` is `VW_ERR_READ` or
/// `VW_ERR_BODY` and zero otherwise; `detail` is the index or byte the error
/// variant carries, or zero.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct VwStatus {
    pub error: i32,
    pub op_error: i32,
    pub detail: u32,
}

impl VwStatus {
    pub(crate) const OK: VwStatus = VwStatus {
        error: VW_OK,
        op_error: 0,
        detail: 0,
    };

    fn plain(error: i32) -> VwStatus {
        VwStatus {
            error,
            op_error: 0,
            detail: 0,
        }
    }

    fn detailed(error: i32, detail: u32) -> VwStatus {
        VwStatus {
            error,
            op_error: 0,
            detail,
        }
    }

    fn nested(error: i32, e: OpError) -> VwStatus {
        VwStatus {
            error,
            op_error: op_error_code(e),
            detail: 0,
        }
    }
}

pub(crate) fn op_error_code(e: OpError) -> i32 {
    match e {
        OpError::UnexpectedEof => VW_OP_ERR_UNEXPECTED_EOF,
        OpError::LebTooLong => VW_OP_ERR_LEB_TOO_LONG,
        OpError::UnrecognizedOpcode => VW_OP_ERR_UNRECOGNIZED_OPCODE,
        OpError::InvalidBlockType => VW_OP_ERR_INVALID_BLOCK_TYPE,
        OpError::TypeMismatch => VW_OP_ERR_TYPE_MISMATCH,
        OpError::EmptyStack => VW_OP_ERR_EMPTY_STACK,
        OpError::StackMismatch => VW_OP_ERR_STACK_MISMATCH,
        OpError::UnknownLabel => VW_OP_ERR_UNKNOWN_LABEL,
        OpError::UnknownLocal => VW_OP_ERR_UNKNOWN_LOCAL,
        OpError::UnknownGlobal => VW_OP_ERR_UNKNOWN_GLOBAL,
        OpError::ImmutableGlobal => VW_OP_ERR_IMMUTABLE_GLOBAL,
        OpError::UnknownMemory => VW_OP_ERR_UNKNOWN_MEMORY,
        OpError::UnknownFunc => VW_OP_ERR_UNKNOWN_FUNC,
        OpError::UnknownType => VW_OP_ERR_UNKNOWN_TYPE,
        OpError::UnknownTable => VW_OP_ERR_UNKNOWN_TABLE,
        OpError::TooManyResults => VW_OP_ERR_TOO_MANY_RESULTS,
        OpError::ReservedByteNotZero => VW_OP_ERR_RESERVED_BYTE_NOT_ZERO,
        OpError::InvalidAlignment => VW_OP_ERR_INVALID_ALIGNMENT,
        OpError::TrailingBytes => VW_OP_ERR_TRAILING_BYTES,
        OpError::BodyTooLarge => VW_OP_ERR_BODY_TOO_LARGE,
        OpError::Visitor(VisitError::OutOfMemory) => VW_OP_ERR_VISITOR_OOM,
    }
}

pub(crate) fn status(e: Error) -> VwStatus {
    status_ref(&e)
}

pub(crate) fn status_ref(e: &Error) -> VwStatus {
    match *e {
        Error::Read(op) => VwStatus::nested(VW_ERR_READ, op),
        Error::Body(op) => VwStatus::nested(VW_ERR_BODY, op),
        Error::Visitor(VisitError::OutOfMemory) => {
            VwStatus::nested(VW_ERR_VISITOR, OpError::Visitor(VisitError::OutOfMemory))
        }
        Error::InvalidMagic => VwStatus::plain(VW_ERR_INVALID_MAGIC),
        Error::InvalidVersion => VwStatus::plain(VW_ERR_INVALID_VERSION),
        Error::UnexpectedEof => VwStatus::plain(VW_ERR_UNEXPECTED_EOF),
        Error::InvalidType(b) => VwStatus::detailed(VW_ERR_INVALID_TYPE, b as u32),
        Error::UnknownSection(id) => VwStatus::detailed(VW_ERR_UNKNOWN_SECTION, id as u32),
        Error::SectionOutOfOrder => VwStatus::plain(VW_ERR_SECTION_OUT_OF_ORDER),
        Error::SectionSizeMismatch => VwStatus::plain(VW_ERR_SECTION_SIZE_MISMATCH),
        Error::FuncCodeMismatch => VwStatus::plain(VW_ERR_FUNC_CODE_MISMATCH),
        Error::MissingCodeSection => VwStatus::plain(VW_ERR_MISSING_CODE_SECTION),
        Error::InvalidUtf8 => VwStatus::plain(VW_ERR_INVALID_UTF8),
        Error::UnknownType(i) => VwStatus::detailed(VW_ERR_UNKNOWN_TYPE, i),
        Error::UnknownFunc(i) => VwStatus::detailed(VW_ERR_UNKNOWN_FUNC, i),
        Error::UnknownTable(i) => VwStatus::detailed(VW_ERR_UNKNOWN_TABLE, i),
        Error::UnknownMemory(i) => VwStatus::detailed(VW_ERR_UNKNOWN_MEMORY, i),
        Error::UnknownGlobal(i) => VwStatus::detailed(VW_ERR_UNKNOWN_GLOBAL, i),
        Error::DuplicateExportName => VwStatus::plain(VW_ERR_DUPLICATE_EXPORT_NAME),
        Error::LimitsOutOfRange => VwStatus::plain(VW_ERR_LIMITS_OUT_OF_RANGE),
        Error::MultipleMemories => VwStatus::plain(VW_ERR_MULTIPLE_MEMORIES),
        Error::MultipleTables => VwStatus::plain(VW_ERR_MULTIPLE_TABLES),
        Error::InvalidStartType => VwStatus::plain(VW_ERR_INVALID_START_TYPE),
        Error::MutableGlobalInConstExpr => {
            VwStatus::plain(VW_ERR_MUTABLE_GLOBAL_IN_CONST_EXPR)
        }
        Error::InvalidConstExpr => VwStatus::plain(VW_ERR_INVALID_CONST_EXPR),
        Error::TooManyResults => VwStatus::plain(VW_ERR_TOO_MANY_RESULTS),
        Error::TooManyLocals => VwStatus::plain(VW_ERR_TOO_MANY_LOCALS),
        Error::ModuleTooLarge => VwStatus::plain(VW_ERR_MODULE_TOO_LARGE),
    }
}

// --- Conversions into the mirrors ---

pub(crate) fn val_type(vt: ValueType) -> VwValType {
    match vt {
        ValueType::I32 => VwValType::I32,
        ValueType::I64 => VwValType::I64,
        ValueType::F32 => VwValType::F32,
        ValueType::F64 => VwValType::F64,
    }
}

pub(crate) fn stack_type(st: StackType) -> VwStackType {
    match st {
        StackType::Val(ValueType::I32) => VwStackType::I32,
        StackType::Val(ValueType::I64) => VwStackType::I64,
        StackType::Val(ValueType::F32) => VwStackType::F32,
        StackType::Val(ValueType::F64) => VwStackType::F64,
        StackType::Bot => VwStackType::Bot,
    }
}

pub(crate) fn block_type(bt: BlockType) -> VwBlockType {
    match bt {
        BlockType::Empty => VwBlockType::Empty,
        BlockType::Value(ValueType::I32) => VwBlockType::I32,
        BlockType::Value(ValueType::I64) => VwBlockType::I64,
        BlockType::Value(ValueType::F32) => VwBlockType::F32,
        BlockType::Value(ValueType::F64) => VwBlockType::F64,
    }
}

pub(crate) fn label_kind(k: LabelKind) -> VwLabelKind {
    match k {
        LabelKind::Body => VwLabelKind::Body,
        LabelKind::Block => VwLabelKind::Block,
        LabelKind::Loop => VwLabelKind::Loop,
        LabelKind::Then => VwLabelKind::Then,
        LabelKind::Else => VwLabelKind::Else,
    }
}

pub(crate) fn mem_arg(m: MemArg) -> VwMemArg {
    VwMemArg {
        align: m.align,
        offset: m.offset,
    }
}

pub(crate) fn limits(l: &Limits) -> VwLimits {
    match l.max {
        Some(max) => VwLimits {
            min: l.min,
            max,
            has_max: true,
        },
        None => VwLimits {
            min: l.min,
            max: 0,
            has_max: false,
        },
    }
}

pub(crate) fn table_type(t: TableType) -> VwTableType {
    VwTableType {
        limits: limits(&t.limits),
    }
}

pub(crate) fn mem_type(m: MemType) -> VwMemType {
    VwMemType {
        limits: limits(&m.limits),
    }
}

pub(crate) fn global_type(g: GlobalType) -> VwGlobalType {
    VwGlobalType {
        valtype: val_type(g.valtype),
        mutability: match g.mutability {
            Mut::Const => VwMut::Const,
            Mut::Var => VwMut::Var,
        },
    }
}

pub(crate) fn const_expr(c: ConstExpr) -> VwConstExpr {
    match c {
        ConstExpr::I32(v) => VwConstExpr {
            kind: VwConstExprKind::I32,
            bits: v as i64 as u64,
        },
        ConstExpr::I64(v) => VwConstExpr {
            kind: VwConstExprKind::I64,
            bits: v as u64,
        },
        ConstExpr::F32(b) => VwConstExpr {
            kind: VwConstExprKind::F32,
            bits: b as u64,
        },
        ConstExpr::F64(b) => VwConstExpr {
            kind: VwConstExprKind::F64,
            bits: b,
        },
        ConstExpr::GlobalGet(i) => VwConstExpr {
            kind: VwConstExprKind::GlobalGet,
            bits: i as u64,
        },
    }
}
