//! A C ABI for itasca, SpiderMonkey's wasm 1.0 validator.
//!
//! This is ordinary Rust with raw pointers, glue between itasca's verified
//! core and the compilers in `js/src/wasm/`. It is not part of itasca's
//! extraction; see itasca's README and DEVELOPMENT.md for the verified boundary.
//!
//! `itasca_validate_stream` is the experimental whole-module entry point. It
//! owns phase sequencing and consumes body-validation batch results before
//! accepting. The implementation is itasca's opt-in `stream` module and is
//! extracted but not yet proved. Retained tier-2 compilation uses the code-entry API.
//!
//! # Safety
//!
//! Every entry point here is `unsafe` for the same three reasons, so they are
//! written down once rather than on each. A `(data, len)` pair must describe a
//! readable range or be `(anything, 0)`. A handle must be one this library
//! produced and has not freed, and the `*mut` out-parameters must be writable.
//! A hook must return, must not unwind, and must return `VW_VISIT_OK` if it
//! wants validation to finish.
//!
//! A null pointer or an out-of-range index aborts the process, with a message
//! naming what was wrong. It is not reported as a rejection and no default is
//! invented for it: those are mistakes in the caller, not things a module can
//! be, and a verdict or a value that came from one would be indistinguishable
//! from a real one. Panicking out of an `extern "C"` function aborts rather than
//! unwinding, which is what makes this safe to do at the boundary. Null is
//! still a no-op for the `_free` functions, since freeing nothing is a thing a
//! caller means.

// The safety contract is documented once, above, rather than restated on each
// of the several dozen entry points.
#![allow(clippy::missing_safety_doc)]

mod baseline;
pub mod env;
mod stream;
pub mod types;
pub mod visitor;

use core::ffi::{c_char, c_void};

use itasca::module;

pub use types::*;
pub use visitor::*;

use baseline::BaselineVisitor;
use visitor::FfiVisitor;

/// A `(data, len)` pair as a slice. A null pointer with a non-zero length
/// aborts.
unsafe fn bytes<'a>(data: *const u8, len: usize) -> &'a [u8] {
    if len == 0 {
        return &[];
    }
    assert!(!data.is_null(), "itasca: null data with a non-zero length");
    core::slice::from_raw_parts(data, len)
}

/// A handle, or an abort naming which one was null.
unsafe fn handle<'a, T>(p: *const T, what: &str) -> &'a T {
    assert!(!p.is_null(), "itasca: {what} must not be null");
    &*p
}

/// An out-parameter, or an abort naming which one was null.
unsafe fn out_ptr<'a, T>(p: *mut T, what: &str) -> &'a mut T {
    assert!(!p.is_null(), "itasca: {what} must not be null");
    &mut *p
}

#[no_mangle]
pub unsafe extern "C" fn itasca_env_free(env: *mut VwEnv) {
    if !env.is_null() {
        drop(Box::from_raw(env));
    }
}

// --- Retained code-entry validation for tier-2 compilation ---

/// Validate one code-section entry, reporting each accepted operator to
/// `visitor`.
///
/// `pos` is at the entry's size prefix and `index` selects its signature from
/// the environment's function list; `*out_next_pos` comes back just past the
/// entry. Several threads may drive different entries against one `env`, which
/// is what a parallel function compiler needs.
///
/// A null `visitor` validates and reports nothing, which is the same run at the
/// do-nothing consumer and is what a validate-only embedder wants. `user` is
/// handed back to every hook untouched.
///
/// `*out_error_offset` is where to point a diagnostic. On a rejected body with a
/// visitor supplied it is the start of the operator that was refused: the last
/// hook to fire reported an operator that was accepted, so the cursor it left
/// behind is the start of the one that was not. Otherwise, and on success, it is
/// `pos`, which brackets the failure to this entry and no closer. It is derived
/// the way `op_offset` is and no theorem mentions it.
#[no_mangle]
pub unsafe extern "C" fn itasca_validate_code_entry(
    data: *const u8,
    len: usize,
    pos: usize,
    env: *const VwEnv,
    index: usize,
    visitor: *const VwVisitor,
    user: *mut c_void,
    out_next_pos: *mut usize,
    out_error_offset: *mut usize,
) -> VwStatus {
    let next_slot = out_ptr(out_next_pos, "out_next_pos");
    let error_slot = out_ptr(out_error_offset, "out_error_offset");
    *next_slot = 0;
    *error_slot = pos;
    let d = bytes(data, len);
    let e = &handle(env, "env").0;
    let r = if visitor.is_null() {
        module::validate_code_entry(d, pos, e, index)
    } else {
        let mut v = FfiVisitor::new(&*visitor, user);
        let r = module::validate_code_entry_with(d, pos, e, index, &mut v);
        if r.is_err() {
            *error_slot = v.error_offset();
        }
        r
    };
    match r {
        Ok(next) => {
            *next_slot = next;
            VwStatus::OK
        }
        Err(err) => status(err),
    }
}

/// Same as `itasca_validate_code_entry`, driven with baseline's hooks.
///
/// Baseline's hooks are fixed `extern "C"` symbols (see
/// `WasmBaselineCompile.cpp`) rather than a `VwVisitor` a caller builds and
/// hands over, so there is no `visitor` parameter: `BaselineVisitor` calls
/// them directly by name, which is what lets them be inlined across the FFI
/// boundary instead of dispatched through a vtable. Baseline never runs
/// without real hooks, so unlike `itasca_validate_code_entry` there is no
/// no-visitor case to fall back to.
#[no_mangle]
pub unsafe extern "C" fn itasca_validate_code_entry_baseline(
    data: *const u8,
    len: usize,
    pos: usize,
    env: *const VwEnv,
    index: usize,
    user: *mut c_void,
    out_next_pos: *mut usize,
    out_error_offset: *mut usize,
) -> VwStatus {
    let next_slot = out_ptr(out_next_pos, "out_next_pos");
    let error_slot = out_ptr(out_error_offset, "out_error_offset");
    *next_slot = 0;
    *error_slot = pos;
    let d = bytes(data, len);
    let e = &handle(env, "env").0;
    let mut v = BaselineVisitor::new(user);
    let r = module::validate_code_entry_with(d, pos, e, index, &mut v);
    if r.is_err() {
        *error_slot = v.error_offset();
    }
    match r {
        Ok(next) => {
            *next_slot = next;
            VwStatus::OK
        }
        Err(err) => status(err),
    }
}

fn code_verdict(r: u32) -> Result<(), itasca::error::Error> {
    if r == VW_VISIT_OK {
        Ok(())
    } else {
        Err(itasca::error::Error::Visitor(
            itasca::error::VisitError::OutOfMemory,
        ))
    }
}

#[no_mangle]
pub unsafe extern "C" fn itasca_tail_free(tail: *mut VwTail) {
    if !tail.is_null() {
        drop(Box::from_raw(tail));
    }
}

// --- Why something was rejected ---

/// A static description of an error code. Never null: an unrecognised code
/// reports that it is unrecognised.
///
/// Errors are unspecified in both the core specification and the web embedding,
/// and no variant carries a proof obligation, so these are for reading and not
/// for deciding anything. The index or byte a variant carries is in
/// `VwStatus::detail` rather than in the string, which is why no formatting and
/// so no allocation happens here.
#[no_mangle]
pub extern "C" fn itasca_error_str(code: i32) -> *const c_char {
    let s = match code {
        VW_OK => c"ok",
        VW_ERR_READ => c"malformed encoding",
        VW_ERR_INVALID_MAGIC => c"invalid magic number",
        VW_ERR_INVALID_VERSION => c"invalid version",
        VW_ERR_UNEXPECTED_EOF => c"unexpected end of input",
        VW_ERR_INVALID_TYPE => c"invalid type byte",
        VW_ERR_UNKNOWN_SECTION => c"unknown section id",
        VW_ERR_SECTION_OUT_OF_ORDER => c"section out of order",
        VW_ERR_SECTION_SIZE_MISMATCH => c"section size does not match its contents",
        VW_ERR_FUNC_CODE_MISMATCH => {
            c"function body count does not match function signature count"
        }
        VW_ERR_INVALID_UTF8 => c"name is not valid UTF-8",
        VW_ERR_UNKNOWN_TYPE => c"unknown type",
        VW_ERR_UNKNOWN_FUNC => c"unknown function",
        VW_ERR_UNKNOWN_TABLE => c"unknown table",
        VW_ERR_UNKNOWN_MEMORY => c"unknown memory",
        VW_ERR_UNKNOWN_GLOBAL => c"unknown global",
        VW_ERR_DUPLICATE_EXPORT_NAME => c"duplicate export name",
        VW_ERR_LIMITS_OUT_OF_RANGE => c"limits out of range",
        VW_ERR_MULTIPLE_MEMORIES => c"multiple memories",
        VW_ERR_MULTIPLE_TABLES => c"multiple tables",
        VW_ERR_INVALID_START_TYPE => c"start function must have type [] -> []",
        VW_ERR_MUTABLE_GLOBAL_IN_CONST_EXPR => {
            c"mutable or non-imported global in constant expression"
        }
        VW_ERR_INVALID_CONST_EXPR => c"invalid constant expression",
        VW_ERR_TOO_MANY_RESULTS => c"a function type returns more than one value",
        VW_ERR_TOO_MANY_LOCALS => c"too many locals",
        VW_ERR_MODULE_TOO_LARGE => c"module too large",
        VW_ERR_BODY => c"invalid function body",
        VW_ERR_MISSING_CODE_SECTION => {
            c"the module declares functions and has no code section"
        }
        VW_ERR_VISITOR => c"the consumer declined",
        _ => c"unrecognized error code",
    };
    s.as_ptr()
}

/// A static description of an operator-level error code, as
/// `VwStatus::op_error` reports it. Never null.
#[no_mangle]
pub extern "C" fn itasca_op_error_str(code: i32) -> *const c_char {
    let s = match code {
        0 => c"ok",
        VW_OP_ERR_UNEXPECTED_EOF => c"unexpected end of input",
        VW_OP_ERR_LEB_TOO_LONG => c"LEB128 encoding too long",
        VW_OP_ERR_UNRECOGNIZED_OPCODE => c"unrecognized opcode",
        VW_OP_ERR_INVALID_BLOCK_TYPE => c"invalid block type",
        VW_OP_ERR_TYPE_MISMATCH => c"type mismatch",
        VW_OP_ERR_EMPTY_STACK => c"empty stack",
        VW_OP_ERR_STACK_MISMATCH => c"stack mismatch",
        VW_OP_ERR_UNKNOWN_LABEL => c"unknown label",
        VW_OP_ERR_UNKNOWN_LOCAL => c"unknown local",
        VW_OP_ERR_UNKNOWN_GLOBAL => c"unknown global",
        VW_OP_ERR_IMMUTABLE_GLOBAL => c"global is immutable",
        VW_OP_ERR_UNKNOWN_MEMORY => c"unknown memory",
        VW_OP_ERR_UNKNOWN_FUNC => c"unknown function",
        VW_OP_ERR_UNKNOWN_TYPE => c"unknown type",
        VW_OP_ERR_UNKNOWN_TABLE => c"unknown table",
        VW_OP_ERR_TOO_MANY_RESULTS => c"a callee returns more than one value",
        VW_OP_ERR_RESERVED_BYTE_NOT_ZERO => c"expected reserved byte to be 0",
        VW_OP_ERR_INVALID_ALIGNMENT => c"invalid alignment",
        VW_OP_ERR_TRAILING_BYTES => c"trailing bytes after the body's end",
        VW_OP_ERR_BODY_TOO_LARGE => c"function body too large",
        VW_OP_ERR_VISITOR_OOM => c"the consumer declined the body",
        _ => c"unrecognized error code",
    };
    s.as_ptr()
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The vtable is one pointer per hook, which is what the header's
    /// static_asserts say from the other side. A hook added to one and not the
    /// other fails here or there.
    #[test]
    fn the_vtable_has_one_slot_per_hook() {
        let ptr = core::mem::size_of::<*const ()>();
        assert_eq!(core::mem::size_of::<VwMemoryHooks>(), 23 * ptr);
        assert_eq!(core::mem::size_of::<VwNumericHooks>(), 123 * ptr);
        assert_eq!(core::mem::size_of::<VwVisitor>(), 174 * ptr);
    }

    /// A hook that declines is a rejection rather than a crash, and the
    /// interface reports it as one. This is the `hooks_accept` side of the
    /// contract seen from outside.
    #[test]
    fn a_declining_consumer_stops_the_body() {
        struct Refuser;
        impl itasca::code::OpVisitor for Refuser {
            fn on_nop(
                &mut self,
                _st: &itasca::code::OpIterState,
            ) -> itasca::code::VisitResult {
                Err(itasca::error::VisitError::OutOfMemory)
            }
        }

        // (module (func nop))
        let wasm: &[u8] = &[
            0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
            0x01, 0x04, 0x01, 0x60, 0x00, 0x00,
            0x03, 0x02, 0x01, 0x00,
            0x0a, 0x05, 0x01, 0x03, 0x00, 0x01, 0x0b,
        ];
        let (env, code_pos) = module::validate_env(wasm).unwrap();
        let cs = module::code_section(wasm, code_pos, &env).unwrap().unwrap();

        assert!(module::validate_code_entry(wasm, cs.entries, &env, 0).is_ok());
        let mut refuser = Refuser;
        let err = module::validate_code_entry_with(wasm, cs.entries, &env, 0, &mut refuser)
            .expect_err("a declining consumer should stop the body");
        assert_eq!(status(err).op_error, VW_OP_ERR_VISITOR_OOM);
    }

    /// A body that fails mid-way is reported at the operator that failed, not
    /// at the start of the entry. This is the offset a compiler puts in its
    /// diagnostic.
    #[test]
    fn a_rejected_body_reports_where_it_failed() {
        // (module (func (result i32) i32.const 1 i64.const 2 i32.add))
        // `i32.add` has an i64 under it, so the failure is at the last operator.
        let wasm: &[u8] = &[
            0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
            0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7f,
            0x03, 0x02, 0x01, 0x00,
            0x0a, 0x09, 0x01, 0x07, 0x00, 0x41, 0x01, 0x42, 0x02, 0x6a, 0x0b,
        ];

        let (decoded, code_pos) = module::validate_env(wasm).unwrap();
        let entries = module::code_section(wasm, code_pos, &decoded).unwrap().unwrap().entries;
        let env = Box::into_raw(Box::new(VwEnv(decoded)));
        let mut next = 0;

        // Every hook default, so no hook declines: the rejection is the
        // validator's.
        let visitor = VwVisitor::default();
        let mut error_at = 0;
        let s = unsafe {
            itasca_validate_code_entry(
                wasm.as_ptr(),
                wasm.len(),
                entries,
                env,
                0,
                &visitor,
                core::ptr::null_mut(),
                &mut next,
                &mut error_at,
            )
        };
        assert_eq!(s.error, VW_ERR_BODY);
        assert_eq!(s.op_error, VW_OP_ERR_TYPE_MISMATCH);

        // The body starts at 24 (past the entry's size prefix and its empty
        // locals declaration), so `i32.const 1` is at 24, `i64.const 2` at 26
        // and the `i32.add` that failed at 28.
        assert_eq!(error_at, 28);
        assert_eq!(wasm[error_at], 0x6a);

        unsafe { itasca_env_free(env) };
    }
}
