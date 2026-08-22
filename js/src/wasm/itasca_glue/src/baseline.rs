//! Baseline's hooks, called directly rather than through a `VwVisitor`.
//!
//! Baseline is hot enough per operator that the indirect call `FfiVisitor`
//! pays for every hook is a real fraction of its compile time; Ion's own
//! optimizing backend dominates its own compile time, so it keeps going
//! through `FfiVisitor`. `WasmBaselineCompile.cpp`'s hooks are fixed
//! `extern "C"` symbols instead of `VwVisitor` entries, so `BaselineVisitor`
//! calls them by name -- something cross-language LTO can fold across the
//! FFI boundary -- instead of reading a function pointer out of a struct at
//! runtime, which it cannot see through.

use core::ffi::c_void;

use itasca::code::{
    memory_table, numeric_table, BlockType, Context, LabelKind, MemArg, OpIterState, OpVisitor,
    StackType, VisitResult,
};

use crate::types::{
    block_type, label_kind, mem_arg, stack_type, VwBlockType, VwContext, VwLabelKind, VwMemArg,
    VwStackType, VwState,
};
use crate::visitor::{context, state, verdict, OpOffsetTracker, VwHookCtx};

// VwState and VwContext are `repr(transparent)` handles that are only ever
// passed as pointers and read back through accessors (see types.rs) -- the
// pointee's layout is never assumed on the C++ side, which is what the
// pointer-to-opaque-handle idiom promises. `improper_ctypes` cannot see that
// through a `repr(transparent)` wrapper over a type with no `repr` of its
// own, since adding one to the verified crate's types is exactly what
// types.rs's own module doc says not to do.
#[allow(improper_ctypes)]
extern "C" {
    fn SM_Baseline_on_function_start(
        c: *const VwHookCtx,
        ctx: *const VwContext,
        type_idx: u32,
        body_begin: usize,
        body_end: usize,
    ) -> u32;
    fn SM_Baseline_on_unreachable(c: *const VwHookCtx, st: *const VwState) -> u32;
    fn SM_Baseline_on_nop(c: *const VwHookCtx, st: *const VwState) -> u32;
    fn SM_Baseline_on_block(c: *const VwHookCtx, st: *const VwState, bt: VwBlockType) -> u32;
    fn SM_Baseline_on_loop(c: *const VwHookCtx, st: *const VwState, bt: VwBlockType) -> u32;
    fn SM_Baseline_on_if(c: *const VwHookCtx, st: *const VwState, bt: VwBlockType) -> u32;
    fn SM_Baseline_on_else(c: *const VwHookCtx, st: *const VwState, bt: VwBlockType) -> u32;
    fn SM_Baseline_on_end(
        c: *const VwHookCtx,
        st: *const VwState,
        kind: VwLabelKind,
        bt: VwBlockType,
    ) -> u32;
    fn SM_Baseline_on_br(c: *const VwHookCtx, st: *const VwState, depth: u32, bt: VwBlockType) -> u32;
    fn SM_Baseline_on_br_if(
        c: *const VwHookCtx,
        st: *const VwState,
        depth: u32,
        bt: VwBlockType,
    ) -> u32;
    fn SM_Baseline_on_br_table_label(c: *const VwHookCtx, st: *const VwState, depth: u32) -> u32;
    fn SM_Baseline_on_br_table(
        c: *const VwHookCtx,
        st: *const VwState,
        default_depth: u32,
        bt: VwBlockType,
    ) -> u32;
    fn SM_Baseline_on_return(c: *const VwHookCtx, st: *const VwState) -> u32;
    fn SM_Baseline_on_call(c: *const VwHookCtx, st: *const VwState, index: u32) -> u32;
    fn SM_Baseline_on_call_indirect(c: *const VwHookCtx, st: *const VwState, index: u32) -> u32;
    fn SM_Baseline_on_drop(c: *const VwHookCtx, st: *const VwState, ty: VwStackType) -> u32;
    fn SM_Baseline_on_select(c: *const VwHookCtx, st: *const VwState, ty: VwStackType) -> u32;
    fn SM_Baseline_on_local_get(c: *const VwHookCtx, st: *const VwState, index: u32) -> u32;
    fn SM_Baseline_on_local_set(c: *const VwHookCtx, st: *const VwState, index: u32) -> u32;
    fn SM_Baseline_on_local_tee(c: *const VwHookCtx, st: *const VwState, index: u32) -> u32;
    fn SM_Baseline_on_global_get(c: *const VwHookCtx, st: *const VwState, index: u32) -> u32;
    fn SM_Baseline_on_global_set(c: *const VwHookCtx, st: *const VwState, index: u32) -> u32;
    fn SM_Baseline_on_memory_size(c: *const VwHookCtx, st: *const VwState) -> u32;
    fn SM_Baseline_on_memory_grow(c: *const VwHookCtx, st: *const VwState) -> u32;
    fn SM_Baseline_on_i32_const(c: *const VwHookCtx, st: *const VwState, v: i32) -> u32;
    fn SM_Baseline_on_i64_const(c: *const VwHookCtx, st: *const VwState, v: i64) -> u32;
    fn SM_Baseline_on_f32_const(c: *const VwHookCtx, st: *const VwState, v: u32) -> u32;
    fn SM_Baseline_on_f64_const(c: *const VwHookCtx, st: *const VwState, v: u64) -> u32;
}

// The 146 memory and numeric hooks, declared from the same tables `FfiVisitor`
// builds its vtable fields from, so this list cannot drift from that one: a
// row added or removed there changes both.
//
// Each is declared under its bare hook name (`on_i32_load`) and linked to its
// `SM_Baseline_`-prefixed C++ symbol via `link_name`, which is what lets
// `forward_baseline_numeric!`/`forward_baseline_memory!` below call it as
// plainly as a hook they wrote themselves.
macro_rules! extern_memory_hooks {
    ($($op:ident => $hook:ident;)*) => {
        #[allow(improper_ctypes)]
        extern "C" {
            $(
                #[link_name = concat!("SM_Baseline_", stringify!($hook))]
                fn $hook(c: *const VwHookCtx, st: *const VwState, m: VwMemArg) -> u32;
            )*
        }
    };
}
memory_table!(extern_memory_hooks);

macro_rules! extern_numeric_hooks {
    ($($op:ident => $hook:ident;)*) => {
        #[allow(improper_ctypes)]
        extern "C" {
            $(
                #[link_name = concat!("SM_Baseline_", stringify!($hook))]
                fn $hook(c: *const VwHookCtx, st: *const VwState) -> u32;
            )*
        }
    };
}
numeric_table!(extern_numeric_hooks);

/// The hooks whose only argument is the state.
macro_rules! forward_plain {
    ($($hook:ident => $extern_fn:ident),* $(,)?) => {
        $(#[inline(always)]
        fn $hook(&mut self, st: &OpIterState) -> VisitResult {
            self.off.begin_op(st);
            verdict(unsafe { $extern_fn(&self.off.ctx, state(st)) })
        })*
    };
}

/// The hooks that carry a block type.
macro_rules! forward_block_type {
    ($($hook:ident => $extern_fn:ident),* $(,)?) => {
        $(#[inline(always)]
        fn $hook(&mut self, st: &OpIterState, bt: BlockType) -> VisitResult {
            self.off.begin_op(st);
            verdict(unsafe { $extern_fn(&self.off.ctx, state(st), block_type(bt)) })
        })*
    };
}

/// The hooks that carry one index.
macro_rules! forward_index {
    ($($hook:ident => $extern_fn:ident),* $(,)?) => {
        $(#[inline(always)]
        fn $hook(&mut self, st: &OpIterState, idx: u32) -> VisitResult {
            self.off.begin_op(st);
            verdict(unsafe { $extern_fn(&self.off.ctx, state(st), idx) })
        })*
    };
}

macro_rules! forward_baseline_numeric {
    ($($op:ident => $hook:ident;)*) => {
        $(#[inline(always)]
        fn $hook(&mut self, st: &OpIterState) -> VisitResult {
            self.off.begin_op(st);
            verdict(unsafe { $hook(&self.off.ctx, state(st)) })
        })*
    };
}

macro_rules! forward_baseline_memory {
    ($($op:ident => $hook:ident;)*) => {
        $(#[inline(always)]
        fn $hook(&mut self, st: &OpIterState, memarg: MemArg) -> VisitResult {
            self.off.begin_op(st);
            verdict(unsafe { $hook(&self.off.ctx, state(st), mem_arg(memarg)) })
        })*
    };
}

/// Drives baseline directly: every hook is a fixed extern "C" call, not a
/// vtable read, so there is no `Option` to match on and no null hook --
/// unlike `FfiVisitor`, which also serves `WebAssembly.validate`'s
/// no-hooks-at-all case, baseline never runs without real hooks.
pub(crate) struct BaselineVisitor {
    off: OpOffsetTracker,
}

impl BaselineVisitor {
    pub(crate) fn new(user: *mut c_void) -> BaselineVisitor {
        BaselineVisitor {
            off: OpOffsetTracker::new(user),
        }
    }

    pub(crate) fn error_offset(&self) -> usize {
        self.off.error_offset()
    }
}

impl OpVisitor for BaselineVisitor {
    #[inline(always)]
    fn on_function_start(
        &mut self,
        ctx: &Context,
        type_idx: u32,
        body_begin: usize,
        body_end: usize,
    ) -> VisitResult {
        self.off.start_function(body_begin);
        verdict(unsafe {
            SM_Baseline_on_function_start(
                &self.off.ctx,
                context(ctx),
                type_idx,
                body_begin,
                body_end,
            )
        })
    }

    forward_plain!(
        on_unreachable => SM_Baseline_on_unreachable,
        on_nop => SM_Baseline_on_nop,
        on_return => SM_Baseline_on_return,
        on_memory_size => SM_Baseline_on_memory_size,
        on_memory_grow => SM_Baseline_on_memory_grow,
    );

    forward_block_type!(
        on_block => SM_Baseline_on_block,
        on_loop => SM_Baseline_on_loop,
        on_if => SM_Baseline_on_if,
        on_else => SM_Baseline_on_else,
    );

    #[inline(always)]
    fn on_end(&mut self, st: &OpIterState, kind: LabelKind, bt: BlockType) -> VisitResult {
        self.off.begin_op(st);
        verdict(unsafe {
            SM_Baseline_on_end(&self.off.ctx, state(st), label_kind(kind), block_type(bt))
        })
    }

    #[inline(always)]
    fn on_br(&mut self, st: &OpIterState, depth: u32, bt: BlockType) -> VisitResult {
        self.off.begin_op(st);
        verdict(unsafe { SM_Baseline_on_br(&self.off.ctx, state(st), depth, block_type(bt)) })
    }

    #[inline(always)]
    fn on_br_if(&mut self, st: &OpIterState, depth: u32, bt: BlockType) -> VisitResult {
        self.off.begin_op(st);
        verdict(unsafe { SM_Baseline_on_br_if(&self.off.ctx, state(st), depth, block_type(bt)) })
    }

    #[inline(always)]
    fn on_br_table(&mut self, st: &OpIterState, depth: u32, bt: BlockType) -> VisitResult {
        self.off.begin_op(st);
        verdict(unsafe {
            SM_Baseline_on_br_table(&self.off.ctx, state(st), depth, block_type(bt))
        })
    }

    #[inline(always)]
    fn on_br_table_label(&mut self, st: &OpIterState, depth: u32) -> VisitResult {
        self.off.within_op();
        verdict(unsafe { SM_Baseline_on_br_table_label(&self.off.ctx, state(st), depth) })
    }

    #[inline(always)]
    fn on_drop(&mut self, st: &OpIterState, ty: StackType) -> VisitResult {
        self.off.begin_op(st);
        verdict(unsafe { SM_Baseline_on_drop(&self.off.ctx, state(st), stack_type(ty)) })
    }

    #[inline(always)]
    fn on_select(&mut self, st: &OpIterState, ty: StackType) -> VisitResult {
        self.off.begin_op(st);
        verdict(unsafe { SM_Baseline_on_select(&self.off.ctx, state(st), stack_type(ty)) })
    }

    #[inline(always)]
    fn on_i32_const(&mut self, st: &OpIterState, v: i32) -> VisitResult {
        self.off.begin_op(st);
        verdict(unsafe { SM_Baseline_on_i32_const(&self.off.ctx, state(st), v) })
    }

    #[inline(always)]
    fn on_i64_const(&mut self, st: &OpIterState, v: i64) -> VisitResult {
        self.off.begin_op(st);
        verdict(unsafe { SM_Baseline_on_i64_const(&self.off.ctx, state(st), v) })
    }

    #[inline(always)]
    fn on_f32_const(&mut self, st: &OpIterState, v: u32) -> VisitResult {
        self.off.begin_op(st);
        verdict(unsafe { SM_Baseline_on_f32_const(&self.off.ctx, state(st), v) })
    }

    #[inline(always)]
    fn on_f64_const(&mut self, st: &OpIterState, v: u64) -> VisitResult {
        self.off.begin_op(st);
        verdict(unsafe { SM_Baseline_on_f64_const(&self.off.ctx, state(st), v) })
    }

    forward_index!(
        on_call => SM_Baseline_on_call,
        on_call_indirect => SM_Baseline_on_call_indirect,
        on_local_get => SM_Baseline_on_local_get,
        on_local_set => SM_Baseline_on_local_set,
        on_local_tee => SM_Baseline_on_local_tee,
        on_global_get => SM_Baseline_on_global_get,
        on_global_set => SM_Baseline_on_global_set,
    );

    numeric_table!(forward_baseline_numeric);
    memory_table!(forward_baseline_memory);
}
