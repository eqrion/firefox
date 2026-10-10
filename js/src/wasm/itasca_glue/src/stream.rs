//! Experimental module driver ABI. A range callback publishes initialized,
//! immutable storage. The caller retains all storage and joins workers before
//! destroying it, including on error. Batch handles have exclusive ownership;
//! submit transfers ownership to C++, complete transfers it back to Rust.
//! finish must join all remaining jobs before the borrowed environment drops.

use crate::baseline::BaselineVisitor;
use crate::types::*;
use crate::visitor::{verdict, FfiVisitor, VwVisitor};
use crate::{bytes, code_verdict, handle, out_ptr};
use core::ffi::c_void;
use core::sync::atomic::{AtomicU64, Ordering};
use itasca::stream::{self, Batch, CodeRange, Config, Host, Region, Result, Source, Visitor};
use itasca::{CustomSection, Env, Error, Tail, VisitError};

pub struct VwBatch<'a>(Batch<'a>);

static NEXT_MODULE_ID: AtomicU64 = AtomicU64::new(1);

fn allocate_module_id(ids: &AtomicU64) -> Result<u64> {
    ids.fetch_update(Ordering::Relaxed, Ordering::Relaxed, |id| id.checked_add(1))
        .map_err(|_| Error::Visitor(VisitError::OutOfMemory))
}

#[repr(u32)]
pub enum VwStreamRegion {
    Environment,
    Code,
    Tail,
}

#[repr(C)]
pub struct VwStreamConfig {
    batch_bytes: usize,
    max_batches: usize,
}
#[repr(C)]
pub struct VwBatchEntry {
    index: usize,
    start: usize,
    contents: usize,
    end: usize,
    data: *const u8,
}
#[repr(C)]
pub struct VwStreamVisitor {
    range:
        extern "C" fn(*mut c_void, VwStreamRegion, usize, usize, *mut *const u8, *mut usize) -> u32,
    environment: extern "C" fn(*mut c_void, *const VwEnv, usize, usize, *mut VwStreamConfig) -> u32,
    custom: extern "C" fn(*mut c_void, VwStreamRegion, usize, usize, usize) -> u32,
    submit: extern "C" fn(*mut c_void, *mut VwBatch) -> u32,
    complete: extern "C" fn(*mut c_void, *mut *mut VwBatch) -> u32,
    code_end: extern "C" fn(*mut c_void) -> u32,
    tail: extern "C" fn(*mut c_void, *mut VwTail) -> u32,
    finish: extern "C" fn(*mut c_void, bool),
}
struct Callbacks<'a> {
    visitor: &'a VwStreamVisitor,
    user: *mut c_void,
}
impl Callbacks<'_> {
    fn acquire(&self, region: VwStreamRegion, start: usize, end: usize) -> Result<&[u8]> {
        let mut data = core::ptr::null();
        let mut len = 0;
        code_verdict((self.visitor.range)(
            self.user, region, start, end, &mut data, &mut len,
        ))?;
        assert!(len == 0 || !data.is_null());
        Ok(unsafe { bytes(data, len) })
    }
}
struct Input<'a> {
    callbacks: &'a Callbacks<'a>,
    code_len: usize,
    prefix: &'a [u8],
}
impl Source for Input<'_> {
    fn prefix(&self) -> Result<&[u8]> {
        Ok(self.prefix)
    }
    fn read(&self, start: usize, end: usize) -> Result<&[u8]> {
        if end < start {
            return Err(Error::UnexpectedEof);
        }
        if self.code_len == 0 || start < self.prefix.len() {
            return self
                .prefix
                .get(start..end.min(self.prefix.len()))
                .ok_or(Error::UnexpectedEof);
        }
        let start = start - self.prefix.len();
        let end = (end - self.prefix.len()).min(self.code_len);
        if start > end {
            return Err(Error::UnexpectedEof);
        }
        self.callbacks.acquire(VwStreamRegion::Code, start, end)
    }
    fn suffix(&self, start: usize) -> Result<&[u8]> {
        if self.code_len == 0 {
            return self.prefix.get(start..).ok_or(Error::UnexpectedEof);
        }
        let expected = self
            .prefix
            .len()
            .checked_add(self.code_len)
            .ok_or(Error::ModuleTooLarge)?;
        if start > expected {
            return Err(Error::UnexpectedEof);
        }
        if start != expected {
            return Err(Error::SectionSizeMismatch);
        }
        self.callbacks.acquire(VwStreamRegion::Tail, 0, 0)
    }
}

struct Stream<'a> {
    callbacks: &'a Callbacks<'a>,
    code_len: usize,
    prefix_len: usize,
    custom_region: Region,
    custom_base: usize,
}

impl itasca::module::ModuleVisitor for Stream<'_> {
    fn on_custom_section(&mut self, s: CustomSection) -> core::result::Result<(), VisitError> {
        let (r, base) = match self.custom_region {
            Region::Environment => (VwStreamRegion::Environment, 0),
            Region::Tail if self.code_len == 0 => (VwStreamRegion::Environment, self.custom_base),
            Region::Tail => (VwStreamRegion::Tail, 0),
        };
        verdict((self.callbacks.visitor.custom)(
            self.callbacks.user,
            r,
            s.name_len,
            base + s.payload_start,
            base + s.payload_end,
        ))
    }
}

struct WorkerGuard<'a> {
    callbacks: &'a Callbacks<'a>,
    success: bool,
}
impl Drop for WorkerGuard<'_> {
    fn drop(&mut self) {
        (self.callbacks.visitor.finish)(self.callbacks.user, self.success);
    }
}

impl<'a> Host<'a> for Stream<'_> {
    fn environment_ready(&mut self, env: &Env, code: Option<CodeRange>) -> Result<Config> {
        let mut config = VwStreamConfig {
            batch_bytes: 1,
            max_batches: 1,
        };
        let (start, len) = match code {
            Some(c) if c.start == self.prefix_len && c.len == self.code_len => (c.start, c.len),
            None if self.code_len == 0 => (0, 0),
            _ => return Err(Error::SectionSizeMismatch),
        };
        let env = env as *const Env as *const VwEnv;
        code_verdict((self.callbacks.visitor.environment)(
            self.callbacks.user,
            env,
            start,
            len,
            &mut config,
        ))?;
        Ok(Config {
            batch_bytes: config.batch_bytes,
            max_batches: config.max_batches,
        })
    }
    fn submit(&mut self, batch: Batch<'a>) -> Result<()> {
        code_verdict((self.callbacks.visitor.submit)(
            self.callbacks.user,
            Box::into_raw(Box::new(VwBatch(batch))),
        ))
    }
    fn complete(&mut self) -> Result<Batch<'a>> {
        let mut batch = core::ptr::null_mut();
        code_verdict((self.callbacks.visitor.complete)(
            self.callbacks.user,
            &mut batch,
        ))?;
        if batch.is_null() {
            return Err(Error::FuncCodeMismatch);
        }
        Ok(unsafe { Box::from_raw(batch) }.0)
    }
    fn code_end(&mut self) -> Result<()> {
        code_verdict((self.callbacks.visitor.code_end)(self.callbacks.user))
    }
    fn tail_ready(&mut self, tail: Tail) -> Result<()> {
        code_verdict((self.callbacks.visitor.tail)(
            self.callbacks.user,
            Box::into_raw(Box::new(VwTail(tail))),
        ))
    }
}
#[no_mangle]
pub unsafe extern "C" fn itasca_validate_stream(
    visitor: *const VwStreamVisitor,
    user: *mut c_void,
    code_len: usize,
    out_env: *mut *mut VwEnv,
) -> VwStatus {
    let callbacks = Callbacks {
        visitor: handle(visitor, "stream visitor"),
        user,
    };
    let mut host = Stream {
        callbacks: &callbacks,
        code_len,
        custom_region: Region::Environment,
        custom_base: 0,
        prefix_len: 0,
    };
    *out_ptr(out_env, "environment") = core::ptr::null_mut();
    // Allocate before borrowing so the address passed to SM remains stable.
    let mut env = Box::new(VwEnv(Env::new()));
    let mut workers = WorkerGuard {
        callbacks: &callbacks,
        success: false,
    };
    let result = match allocate_module_id(&NEXT_MODULE_ID) {
        Ok(id) => match callbacks.acquire(VwStreamRegion::Environment, 0, 0) {
            Ok(prefix) => {
                host.prefix_len = prefix.len();
                let input = Input {
                    callbacks: &callbacks,
                    code_len,
                    prefix,
                };
                stream::validate(id, &input, &mut host, &mut env.0)
            }
            Err(error) => Err(error),
        },
        Err(error) => Err(error),
    };
    workers.success = result.is_ok();
    drop(workers);
    match result {
        Ok(()) => {
            *out_env = Box::into_raw(env);
            VwStatus::OK
        }
        Err(e) => status(e),
    }
}
#[no_mangle]
pub unsafe extern "C" fn itasca_batch_free(batch: *mut VwBatch) {
    if !batch.is_null() {
        drop(Box::from_raw(batch));
    }
}
#[no_mangle]
pub unsafe extern "C" fn itasca_batch_len(batch: *const VwBatch) -> usize {
    handle(batch, "batch").0.len()
}
#[no_mangle]
pub unsafe extern "C" fn itasca_batch_next_entry(
    batch: *mut VwBatch,
    out: *mut VwBatchEntry,
) -> VwStatus {
    let b = &mut out_ptr(batch, "batch").0;
    match b.next_entry() {
        Ok(e) => {
            *out_ptr(out, "entry") = VwBatchEntry {
                index: e.index,
                start: e.start,
                contents: e.contents,
                end: e.end,
                data: b.bytes().as_ptr().add(e.start - b.start()),
            };
            VwStatus::OK
        }
        Err(e) => status(e),
    }
}
#[no_mangle]
pub unsafe extern "C" fn itasca_batch_validate_func(
    batch: *mut VwBatch,
    index: usize,
    visitor: *const VwVisitor,
    user: *mut c_void,
    error_offset: *mut usize,
) -> VwStatus {
    let b = out_ptr(batch, "batch");
    let mut v = FfiVisitor::new(handle(visitor, "visitor"), user);
    let r = b.0.validate_func(index, &mut v);
    *out_ptr(error_offset, "error offset") = v.error_offset();
    match r {
        Ok(()) => VwStatus::OK,
        Err(e) => status_ref(e),
    }
}
#[no_mangle]
pub unsafe extern "C" fn itasca_batch_validate_func_baseline(
    batch: *mut VwBatch,
    index: usize,
    user: *mut c_void,
    error_offset: *mut usize,
) -> VwStatus {
    let b = out_ptr(batch, "batch");
    let mut v = BaselineVisitor::new(user);
    let r = b.0.validate_func(index, &mut v);
    *out_ptr(error_offset, "error offset") = v.error_offset();
    match r {
        Ok(()) => VwStatus::OK,
        Err(e) => status_ref(e),
    }
}

#[no_mangle]
pub unsafe extern "C" fn itasca_batch_is_last(batch: *const VwBatch) -> bool {
    handle(batch, "batch").0.is_last()
}

impl Visitor for Stream<'_> {
    fn custom_region(&mut self, region: Region, base: usize) {
        self.custom_region = region;
        self.custom_base = base;
    }
}

#[cfg(test)]
#[path = "stream_tests.rs"]
mod tests;
