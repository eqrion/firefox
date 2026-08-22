//! The C vtable, and the `OpVisitor` that forwards to it.
//!
//! One function pointer per hook, 1:1 with the trait, so nothing here decides
//! which hook an opcode belongs to and there is no table to get out of step
//! with itasca's `code.rs`. The 146 hooks that come from a table there come
//! from the same table here, via `itasca::code::{memory_table,
//! numeric_table}`.
//!
//! Every pointer is nullable, which is what `Option<extern "C" fn>` means
//! across the boundary: a consumer zero-initialises the struct and fills in
//! only the hooks it wants, so an unused hook still costs nothing. That is the
//! property monomorphisation gives the Rust consumer.
//!
//! What a hook has to do is what `hooks_total` and `hooks_accept` in
//! `OpIter_Visit.v` say: return, and return `VW_VISIT_OK`. A hook that does
//! neither can refuse a valid body or hang, and itasca's TRUST.md is where
//! that is written down. It cannot make the validator accept bad bytes,
//! because acceptance is decided before a hook is consulted. Unwinding out of
//! a hook is undefined behaviour, as it is for any `extern "C"` boundary.

use core::ffi::c_void;

use itasca::code::{
    memory_table, numeric_table, BlockType, Context, LabelKind, MemArg, OpIterState, OpVisitor,
    StackType, VisitResult,
};
use itasca::error::VisitError;

use crate::types::{
    block_type, label_kind, mem_arg, stack_type, val_type, VwBlockType, VwContext, VwLabelKind,
    VwMemArg, VwStackType, VwState, VwValType, VW_VISIT_OK,
};

/// What every hook receives first: the consumer's own pointer, and where the
/// operator being reported starts.
///
/// `op_offset` is here rather than on `VwState` because it is derived rather
/// than read. A hook sees `st` positioned *after* the operator's immediates,
/// but a compiler keys its trap sites, call sites and stack maps on the offset
/// of the operator's start, so this tracks it: the cursor one hook sees is the
/// start of the operator the next hook sees. `on_function_start` supplies the
/// body's base, so the result is an offset into the whole of what the caller
/// handed over and not one into the body.
#[repr(C)]
pub struct VwHookCtx {
    pub user: *mut c_void,
    /// Byte offset of this operator within the bytes handed to
    /// `itasca_validate_code_entry`, which is the same frame as its `pos`
    /// and its `*out_error_offset`. That is the module's own bytes only for a
    /// caller that handed over the whole module; one that handed over a region
    /// adds the region's offset itself.
    pub op_offset: usize,
}

macro_rules! define_numeric_hooks {
    ($($op:ident => $hook:ident;)*) => {
        /// The numeric hooks, generated from `itasca::code::numeric_table!`.
        /// No immediates, so they all have one shape.
        #[repr(C)]
        #[derive(Default)]
        pub struct VwNumericHooks {
            $(pub $hook: Option<extern "C" fn(*const VwHookCtx, *const VwState) -> u32>,)*
        }
    };
}

macro_rules! define_memory_hooks {
    ($($op:ident => $hook:ident;)*) => {
        /// The load and store hooks, generated from `itasca::code::memory_table!`.
        /// One `memarg` each, already checked.
        #[repr(C)]
        #[derive(Default)]
        pub struct VwMemoryHooks {
            $(pub $hook: Option<
                extern "C" fn(*const VwHookCtx, *const VwState, VwMemArg) -> u32
            >,)*
        }
    };
}

numeric_table!(define_numeric_hooks);
memory_table!(define_memory_hooks);

/// The hooks a consumer fills in. The two table-generated groups are nested
/// rather than flattened, because a `macro_rules!` invocation cannot stand in
/// struct-field position, and because the split says which hooks the
/// opcode-to-hook proof covers as a table.
#[repr(C)]
#[derive(Default)]
pub struct VwVisitor {
    // --- The function around the operators ---
    pub on_function_start: Option<
        extern "C" fn(*const VwHookCtx, *const VwContext, u32, usize, usize) -> u32,
    >,

    // --- Control ---
    pub on_unreachable: Option<extern "C" fn(*const VwHookCtx, *const VwState) -> u32>,
    pub on_nop: Option<extern "C" fn(*const VwHookCtx, *const VwState) -> u32>,
    pub on_block: Option<extern "C" fn(*const VwHookCtx, *const VwState, VwBlockType) -> u32>,
    pub on_loop: Option<extern "C" fn(*const VwHookCtx, *const VwState, VwBlockType) -> u32>,
    pub on_if: Option<extern "C" fn(*const VwHookCtx, *const VwState, VwBlockType) -> u32>,
    pub on_else: Option<extern "C" fn(*const VwHookCtx, *const VwState, VwBlockType) -> u32>,
    pub on_end: Option<
        extern "C" fn(*const VwHookCtx, *const VwState, VwLabelKind, VwBlockType) -> u32,
    >,
    pub on_br:
        Option<extern "C" fn(*const VwHookCtx, *const VwState, u32, VwBlockType) -> u32>,
    pub on_br_if:
        Option<extern "C" fn(*const VwHookCtx, *const VwState, u32, VwBlockType) -> u32>,
    pub on_br_table_label: Option<extern "C" fn(*const VwHookCtx, *const VwState, u32) -> u32>,
    pub on_br_table:
        Option<extern "C" fn(*const VwHookCtx, *const VwState, u32, VwBlockType) -> u32>,
    pub on_return: Option<extern "C" fn(*const VwHookCtx, *const VwState) -> u32>,
    pub on_call: Option<extern "C" fn(*const VwHookCtx, *const VwState, u32) -> u32>,
    pub on_call_indirect: Option<extern "C" fn(*const VwHookCtx, *const VwState, u32) -> u32>,

    // --- Parametric ---
    pub on_drop: Option<extern "C" fn(*const VwHookCtx, *const VwState, VwStackType) -> u32>,
    pub on_select: Option<extern "C" fn(*const VwHookCtx, *const VwState, VwStackType) -> u32>,

    // --- Variables ---
    pub on_local_get: Option<extern "C" fn(*const VwHookCtx, *const VwState, u32) -> u32>,
    pub on_local_set: Option<extern "C" fn(*const VwHookCtx, *const VwState, u32) -> u32>,
    pub on_local_tee: Option<extern "C" fn(*const VwHookCtx, *const VwState, u32) -> u32>,
    pub on_global_get: Option<extern "C" fn(*const VwHookCtx, *const VwState, u32) -> u32>,
    pub on_global_set: Option<extern "C" fn(*const VwHookCtx, *const VwState, u32) -> u32>,

    // --- Memory size ---
    pub on_memory_size: Option<extern "C" fn(*const VwHookCtx, *const VwState) -> u32>,
    pub on_memory_grow: Option<extern "C" fn(*const VwHookCtx, *const VwState) -> u32>,

    // --- Constants ---
    pub on_i32_const: Option<extern "C" fn(*const VwHookCtx, *const VwState, i32) -> u32>,
    pub on_i64_const: Option<extern "C" fn(*const VwHookCtx, *const VwState, i64) -> u32>,
    pub on_f32_const: Option<extern "C" fn(*const VwHookCtx, *const VwState, u32) -> u32>,
    pub on_f64_const: Option<extern "C" fn(*const VwHookCtx, *const VwState, u64) -> u32>,

    // --- The two table-generated groups ---
    pub memory: VwMemoryHooks,
    pub numeric: VwNumericHooks,
}

pub(crate) fn state(st: &OpIterState) -> *const VwState {
    st as *const OpIterState as *const VwState
}

pub(crate) fn context(ctx: &Context) -> *const VwContext {
    ctx as *const Context as *const VwContext
}

/// A hook's answer as a validator verdict. Any non-zero value is a decline, so
/// a consumer that returns something other than the two constants is read as
/// having given up rather than as having succeeded.
pub(crate) fn verdict(r: u32) -> VisitResult {
    if r == VW_VISIT_OK {
        Ok(())
    } else {
        Err(VisitError::OutOfMemory)
    }
}

/// The per-call bookkeeping any `OpVisitor` driving hooks over `VwHookCtx`
/// needs: where the operator being reported starts, and where the body was
/// rejected if it was. Shared by `FfiVisitor` and `baseline::BaselineVisitor`
/// so the two dispatch mechanisms (vtable and direct call) don't each carry
/// their own copy of this arithmetic.
pub(crate) struct OpOffsetTracker {
    pub(crate) ctx: VwHookCtx,
    /// Where the next operator starts, which is where the last one's immediates
    /// ended. Relative to the body, as the validator's cursor is.
    next_op_start: usize,
    /// Where the body's bytes start in the module, from `on_function_start`.
    body_base: usize,
}

impl OpOffsetTracker {
    pub(crate) fn new(user: *mut c_void) -> OpOffsetTracker {
        OpOffsetTracker {
            ctx: VwHookCtx {
                user,
                op_offset: 0,
            },
            next_op_start: 0,
            body_base: 0,
        }
    }

    /// A fresh body: the first operator is at its base.
    pub(crate) fn start_function(&mut self, body_begin: usize) {
        self.body_base = body_begin;
        self.next_op_start = 0;
    }

    /// An operator is complete. It started where the previous one ended, and
    /// the cursor is now at the start of the next.
    pub(crate) fn begin_op(&mut self, st: &OpIterState) {
        self.ctx.op_offset = self.body_base + self.next_op_start;
        self.next_op_start = st.pos;
    }

    /// Where the operator being read when validation stopped begins.
    ///
    /// The last hook that fired reported an operator that was accepted, so the
    /// cursor it left is the start of the one that was not. For a body that was
    /// rejected, this is the offset a compiler wants to name in its diagnostic.
    pub(crate) fn error_offset(&self) -> usize {
        self.body_base + self.next_op_start
    }

    /// A report from inside an operator: `br_table`'s label vector arrives one
    /// depth at a time as the loop reads it, so the operator is not over and
    /// the cursor must not move on.
    pub(crate) fn within_op(&mut self) {
        self.ctx.op_offset = self.body_base + self.next_op_start;
    }
}

/// Forwards each hook to the matching function pointer, if there is one.
pub(crate) struct FfiVisitor<'a> {
    vt: &'a VwVisitor,
    off: OpOffsetTracker,
}

impl<'a> FfiVisitor<'a> {
    pub(crate) fn new(vt: &'a VwVisitor, user: *mut c_void) -> FfiVisitor<'a> {
        FfiVisitor {
            vt,
            off: OpOffsetTracker::new(user),
        }
    }

    pub(crate) fn error_offset(&self) -> usize {
        self.off.error_offset()
    }
}

/// The hooks whose only argument is the state.
macro_rules! forward_plain {
    ($($hook:ident),* $(,)?) => {
        $(fn $hook(&mut self, st: &OpIterState) -> VisitResult {
            let f = self.vt.$hook;
            self.off.begin_op(st);
            match f {
                Some(f) => verdict(f(&self.off.ctx, state(st))),
                None => Ok(()),
            }
        })*
    };
}

/// The hooks that carry a block type.
macro_rules! forward_block_type {
    ($($hook:ident),* $(,)?) => {
        $(fn $hook(&mut self, st: &OpIterState, bt: BlockType) -> VisitResult {
            let f = self.vt.$hook;
            self.off.begin_op(st);
            match f {
                Some(f) => verdict(f(&self.off.ctx, state(st), block_type(bt))),
                None => Ok(()),
            }
        })*
    };
}

/// The hooks that carry one index.
macro_rules! forward_index {
    ($($hook:ident),* $(,)?) => {
        $(fn $hook(&mut self, st: &OpIterState, idx: u32) -> VisitResult {
            let f = self.vt.$hook;
            self.off.begin_op(st);
            match f {
                Some(f) => verdict(f(&self.off.ctx, state(st), idx)),
                None => Ok(()),
            }
        })*
    };
}

/// The hooks that carry a depth and the target's block type.
macro_rules! forward_branch {
    ($($hook:ident),* $(,)?) => {
        $(fn $hook(&mut self, st: &OpIterState, depth: u32, bt: BlockType) -> VisitResult {
            let f = self.vt.$hook;
            self.off.begin_op(st);
            match f {
                Some(f) => verdict(f(&self.off.ctx, state(st), depth, block_type(bt))),
                None => Ok(()),
            }
        })*
    };
}

/// The hooks that carry the type that came off the stack.
macro_rules! forward_stack_type {
    ($($hook:ident),* $(,)?) => {
        $(fn $hook(&mut self, st: &OpIterState, ty: StackType) -> VisitResult {
            let f = self.vt.$hook;
            self.off.begin_op(st);
            match f {
                Some(f) => verdict(f(&self.off.ctx, state(st), stack_type(ty))),
                None => Ok(()),
            }
        })*
    };
}

/// The constant hooks, whose immediate is already the right type for C.
macro_rules! forward_const {
    ($($hook:ident($ty:ty)),* $(,)?) => {
        $(fn $hook(&mut self, st: &OpIterState, v: $ty) -> VisitResult {
            let f = self.vt.$hook;
            self.off.begin_op(st);
            match f {
                Some(f) => verdict(f(&self.off.ctx, state(st), v)),
                None => Ok(()),
            }
        })*
    };
}

macro_rules! forward_numeric {
    ($($op:ident => $hook:ident;)*) => {
        $(fn $hook(&mut self, st: &OpIterState) -> VisitResult {
            let f = self.vt.numeric.$hook;
            self.off.begin_op(st);
            match f {
                Some(f) => verdict(f(&self.off.ctx, state(st))),
                None => Ok(()),
            }
        })*
    };
}

macro_rules! forward_memory {
    ($($op:ident => $hook:ident;)*) => {
        $(fn $hook(&mut self, st: &OpIterState, memarg: MemArg) -> VisitResult {
            let f = self.vt.memory.$hook;
            self.off.begin_op(st);
            match f {
                Some(f) => verdict(f(&self.off.ctx, state(st), mem_arg(memarg))),
                None => Ok(()),
            }
        })*
    };
}

impl OpVisitor for FfiVisitor<'_> {
    fn on_function_start(
        &mut self,
        ctx: &Context,
        type_idx: u32,
        body_begin: usize,
        body_end: usize,
    ) -> VisitResult {
        self.off.start_function(body_begin);
        match self.vt.on_function_start {
            Some(f) => verdict(f(
                &self.off.ctx,
                context(ctx),
                type_idx,
                body_begin,
                body_end,
            )),
            None => Ok(()),
        }
    }

    forward_plain!(
        on_unreachable,
        on_nop,
        on_return,
        on_memory_size,
        on_memory_grow,
    );

    forward_block_type!(on_block, on_loop, on_if, on_else);

    fn on_end(&mut self, st: &OpIterState, kind: LabelKind, bt: BlockType) -> VisitResult {
        let f = self.vt.on_end;
        self.off.begin_op(st);
        match f {
            Some(f) => verdict(f(&self.off.ctx, state(st), label_kind(kind), block_type(bt))),
            None => Ok(()),
        }
    }

    forward_branch!(on_br, on_br_if, on_br_table);

    fn on_br_table_label(&mut self, st: &OpIterState, depth: u32) -> VisitResult {
        let f = self.vt.on_br_table_label;
        self.off.within_op();
        match f {
            Some(f) => verdict(f(&self.off.ctx, state(st), depth)),
            None => Ok(()),
        }
    }

    forward_index!(
        on_call,
        on_call_indirect,
        on_local_get,
        on_local_set,
        on_local_tee,
        on_global_get,
        on_global_set,
    );

    forward_stack_type!(on_drop, on_select);

    forward_const!(
        on_i32_const(i32),
        on_i64_const(i64),
        on_f32_const(u32),
        on_f64_const(u64),
    );

    memory_table!(forward_memory);
    numeric_table!(forward_numeric);
}

// --- Reading the function context ---
//
// A null pointer or an out-of-range index aborts. There is no benign default for
// "the stack is not that deep": every value a consumer could be handed instead
// is one it could legitimately have been handed, so a default would be a wrong
// answer it could not detect.

unsafe fn read_ctx<'a>(p: *const VwContext) -> &'a Context {
    assert!(!p.is_null(), "itasca: context must not be null");
    &(*p).0
}

/// The function's params followed by the locals it declared, flattened.
#[no_mangle]
pub unsafe extern "C" fn itasca_context_num_locals(c: *const VwContext) -> usize {
    read_ctx(c).locals.len()
}

#[no_mangle]
pub unsafe extern "C" fn itasca_context_local(c: *const VwContext, i: usize) -> VwValType {
    val_type(read_ctx(c).locals[i])
}
