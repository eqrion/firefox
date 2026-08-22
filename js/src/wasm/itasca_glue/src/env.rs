//! Reading a validated module from C++.
//!
//! Index accessors rather than borrowed slices, because none of itasca's own
//! types is `repr`-annotated and annotating a verified type to suit this
//! interface would be the interface reaching into the proofs. Names and data
//! segment contents are the exception: those really are `[u8]`, so they come
//! back as a pointer and a length into itasca's own allocation, borrowed
//! until the handle is freed.
//!
//! A null handle or an out-of-range index aborts. Every getter indexes
//! directly, so reading past a count is a panic and not a value: a default
//! would be indistinguishable from a real answer, and the count is right there
//! to be read first.

use itasca::module;
use itasca::module::{Env, ExportDesc, ImportDesc};

use crate::types::{
    const_expr, global_type, mem_type, table_type, val_type, VwConstExpr, VwEnv, VwExternKind,
    VwGlobalType, VwMemType, VwTableType, VwTail, VwValType,
};

unsafe fn env<'a>(p: *const VwEnv) -> &'a Env {
    assert!(!p.is_null(), "itasca: env must not be null");
    &(*p).0
}

unsafe fn tail<'a>(p: *const VwTail) -> &'a module::Tail {
    assert!(!p.is_null(), "itasca: tail must not be null");
    &(*p).0
}

/// A borrowed byte range, written out the way C wants it.
unsafe fn byte_range(bytes: &[u8], out_len: *mut usize) -> *const u8 {
    assert!(!out_len.is_null(), "itasca: out_len must not be null");
    *out_len = bytes.len();
    bytes.as_ptr()
}

// --- The type section, which is also the type index space ---

#[no_mangle]
pub unsafe extern "C" fn itasca_env_num_types(e: *const VwEnv) -> usize {
    env(e).types.len()
}

#[no_mangle]
pub unsafe extern "C" fn itasca_env_type_num_params(
    e: *const VwEnv,
    type_idx: usize,
) -> usize {
    env(e).types[type_idx].params.len()
}

#[no_mangle]
pub unsafe extern "C" fn itasca_env_type_param(
    e: *const VwEnv,
    type_idx: usize,
    i: usize,
) -> VwValType {
    val_type(env(e).types[type_idx].params[i])
}

#[no_mangle]
pub unsafe extern "C" fn itasca_env_type_num_results(
    e: *const VwEnv,
    type_idx: usize,
) -> usize {
    env(e).types[type_idx].results.len()
}

#[no_mangle]
pub unsafe extern "C" fn itasca_env_type_result(
    e: *const VwEnv,
    type_idx: usize,
    i: usize,
) -> VwValType {
    val_type(env(e).types[type_idx].results[i])
}

// --- The index spaces of spec 3.1.1: imports first, then definitions ---

#[no_mangle]
pub unsafe extern "C" fn itasca_env_num_funcs(e: *const VwEnv) -> usize {
    env(e).func_types.len()
}

/// At most one, in Wasm 1.0.
#[no_mangle]
pub unsafe extern "C" fn itasca_env_num_tables(e: *const VwEnv) -> usize {
    env(e).table_types.len()
}

#[no_mangle]
pub unsafe extern "C" fn itasca_env_table(e: *const VwEnv, i: usize) -> VwTableType {
    table_type(env(e).table_types[i])
}

/// At most one, in Wasm 1.0.
#[no_mangle]
pub unsafe extern "C" fn itasca_env_num_memories(e: *const VwEnv) -> usize {
    env(e).mem_types.len()
}

#[no_mangle]
pub unsafe extern "C" fn itasca_env_memory(e: *const VwEnv, i: usize) -> VwMemType {
    mem_type(env(e).mem_types[i])
}

#[no_mangle]
pub unsafe extern "C" fn itasca_env_num_globals(e: *const VwEnv) -> usize {
    env(e).global_types.len()
}

// --- The function section: one type index per code-section entry ---

#[no_mangle]
pub unsafe extern "C" fn itasca_env_num_func_defs(e: *const VwEnv) -> usize {
    env(e).func_type_indices.len()
}

/// The type index of the `i`th code-section entry, which is also the `index`
/// argument `itasca_validate_code_entry` takes.
#[no_mangle]
pub unsafe extern "C" fn itasca_env_func_def_type_index(
    e: *const VwEnv,
    i: usize,
) -> u32 {
    env(e).func_type_indices[i]
}

// --- Imports ---

#[no_mangle]
pub unsafe extern "C" fn itasca_env_num_imports(e: *const VwEnv) -> usize {
    env(e).imports.len()
}

/// The import's module name, borrowed. Raw bytes, checked to be valid UTF-8 as
/// they were decoded (spec 5.2.4) but not carried as a `str`.
#[no_mangle]
pub unsafe extern "C" fn itasca_env_import_module(
    e: *const VwEnv,
    i: usize,
    out_len: *mut usize,
) -> *const u8 {
    byte_range(&env(e).imports[i].module, out_len)
}

/// The import's field name, borrowed.
#[no_mangle]
pub unsafe extern "C" fn itasca_env_import_name(
    e: *const VwEnv,
    i: usize,
    out_len: *mut usize,
) -> *const u8 {
    byte_range(&env(e).imports[i].name, out_len)
}

#[no_mangle]
pub unsafe extern "C" fn itasca_env_import_kind(
    e: *const VwEnv,
    i: usize,
) -> VwExternKind {
    match env(e).imports[i].desc {
        ImportDesc::Func(_) => VwExternKind::Func,
        ImportDesc::Table(_) => VwExternKind::Table,
        ImportDesc::Memory(_) => VwExternKind::Memory,
        ImportDesc::Global(_) => VwExternKind::Global,
    }
}

/// The type index the import names. Aborts unless the kind is `Func`.
#[no_mangle]
pub unsafe extern "C" fn itasca_env_import_func_type_index(
    e: *const VwEnv,
    i: usize,
) -> u32 {
    match env(e).imports[i].desc {
        ImportDesc::Func(t) => t,
        _ => panic!("itasca: import {i} is not a function"),
    }
}

/// Aborts unless the kind is `Table`.
#[no_mangle]
pub unsafe extern "C" fn itasca_env_import_table(
    e: *const VwEnv,
    i: usize,
) -> VwTableType {
    match env(e).imports[i].desc {
        ImportDesc::Table(t) => table_type(t),
        _ => panic!("itasca: import {i} is not a table"),
    }
}

/// Aborts unless the kind is `Memory`.
#[no_mangle]
pub unsafe extern "C" fn itasca_env_import_memory(
    e: *const VwEnv,
    i: usize,
) -> VwMemType {
    match env(e).imports[i].desc {
        ImportDesc::Memory(m) => mem_type(m),
        _ => panic!("itasca: import {i} is not a memory"),
    }
}

/// Aborts unless the kind is `Global`.
#[no_mangle]
pub unsafe extern "C" fn itasca_env_import_global(
    e: *const VwEnv,
    i: usize,
) -> VwGlobalType {
    match env(e).imports[i].desc {
        ImportDesc::Global(g) => global_type(g),
        _ => panic!("itasca: import {i} is not a global"),
    }
}

// --- The globals this module defines ---

#[no_mangle]
pub unsafe extern "C" fn itasca_env_num_defined_globals(e: *const VwEnv) -> usize {
    env(e).globals.len()
}

#[no_mangle]
pub unsafe extern "C" fn itasca_env_defined_global_type(
    e: *const VwEnv,
    i: usize,
) -> VwGlobalType {
    global_type(env(e).globals[i].gtype)
}

#[no_mangle]
pub unsafe extern "C" fn itasca_env_defined_global_init(
    e: *const VwEnv,
    i: usize,
) -> VwConstExpr {
    const_expr(env(e).globals[i].init)
}

// --- Exports ---

#[no_mangle]
pub unsafe extern "C" fn itasca_env_num_exports(e: *const VwEnv) -> usize {
    env(e).exports.len()
}

#[no_mangle]
pub unsafe extern "C" fn itasca_env_export_name(
    e: *const VwEnv,
    i: usize,
    out_len: *mut usize,
) -> *const u8 {
    byte_range(&env(e).exports[i].name, out_len)
}

#[no_mangle]
pub unsafe extern "C" fn itasca_env_export_kind(
    e: *const VwEnv,
    i: usize,
) -> VwExternKind {
    match env(e).exports[i].desc {
        ExportDesc::Func(_) => VwExternKind::Func,
        ExportDesc::Table(_) => VwExternKind::Table,
        ExportDesc::Memory(_) => VwExternKind::Memory,
        ExportDesc::Global(_) => VwExternKind::Global,
    }
}

/// The index the export names, in the index space its kind selects.
#[no_mangle]
pub unsafe extern "C" fn itasca_env_export_index(e: *const VwEnv, i: usize) -> u32 {
    match env(e).exports[i].desc {
        ExportDesc::Func(j) => j,
        ExportDesc::Table(j) => j,
        ExportDesc::Memory(j) => j,
        ExportDesc::Global(j) => j,
    }
}

// --- The start function ---

#[no_mangle]
pub unsafe extern "C" fn itasca_env_has_start(e: *const VwEnv) -> bool {
    env(e).start.is_some()
}

/// Aborts unless `itasca_env_has_start`.
#[no_mangle]
pub unsafe extern "C" fn itasca_env_start(e: *const VwEnv) -> u32 {
    match env(e).start {
        Some(i) => i,
        None => panic!("itasca: the module has no start function"),
    }
}

// --- Element segments ---

#[no_mangle]
pub unsafe extern "C" fn itasca_env_num_elements(e: *const VwEnv) -> usize {
    env(e).elements.len()
}

#[no_mangle]
pub unsafe extern "C" fn itasca_env_element_table_index(
    e: *const VwEnv,
    i: usize,
) -> u32 {
    env(e).elements[i].table_idx
}

#[no_mangle]
pub unsafe extern "C" fn itasca_env_element_offset(
    e: *const VwEnv,
    i: usize,
) -> VwConstExpr {
    const_expr(env(e).elements[i].offset)
}

#[no_mangle]
pub unsafe extern "C" fn itasca_env_element_num_funcs(
    e: *const VwEnv,
    i: usize,
) -> usize {
    env(e).elements[i].init.len()
}

#[no_mangle]
pub unsafe extern "C" fn itasca_env_element_func(
    e: *const VwEnv,
    i: usize,
    j: usize,
) -> u32 {
    env(e).elements[i].init[j]
}

// --- Data segments, from the tail ---

#[no_mangle]
pub unsafe extern "C" fn itasca_tail_num_data(t: *const VwTail) -> usize {
    tail(t).data.len()
}

#[no_mangle]
pub unsafe extern "C" fn itasca_tail_data_memory_index(
    t: *const VwTail,
    i: usize,
) -> u32 {
    tail(t).data[i].memory_idx
}

#[no_mangle]
pub unsafe extern "C" fn itasca_tail_data_offset(
    t: *const VwTail,
    i: usize,
) -> VwConstExpr {
    const_expr(tail(t).data[i].offset)
}

/// The segment's contents, borrowed until the tail is freed.
#[no_mangle]
pub unsafe extern "C" fn itasca_tail_data_bytes(
    t: *const VwTail,
    i: usize,
    out_len: *mut usize,
) -> *const u8 {
    byte_range(&tail(t).data[i].init, out_len)
}
