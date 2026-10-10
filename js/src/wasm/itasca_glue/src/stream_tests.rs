use super::*;

#[test]
fn module_ids_never_wrap_or_reuse() {
    let ids = AtomicU64::new(u64::MAX - 1);
    assert_eq!(allocate_module_id(&ids).unwrap(), u64::MAX - 1);
    assert!(allocate_module_id(&ids).is_err());
    assert!(allocate_module_id(&ids).is_err());
    assert_eq!(ids.load(Ordering::Relaxed), u64::MAX);
}

#[derive(Clone, Copy, PartialEq)]
enum Stop {
    Never,
    Input,
    Environment,
    Submit,
    Complete,
    Tail,
}
struct TestStream {
    stop: Stop,
    env: *const VwEnv,
    batch: *mut VwBatch<'static>,
    finishes: usize,
    success: bool,
}
unsafe fn state<'a>(user: *mut c_void) -> &'a mut TestStream {
    unsafe { &mut *user.cast() }
}
extern "C" fn range(
    user: *mut c_void,
    region: VwStreamRegion,
    start: usize,
    end: usize,
    data: *mut *const u8,
    len: *mut usize,
) -> u32 {
    if unsafe { state(user) }.stop == Stop::Input {
        return 1;
    }
    let bytes: &[u8] = match region {
        VwStreamRegion::Environment => b"\0asm\x01\0\0\0\x01\x04\x01\x60\0\0\x03\x02\x01\0",
        VwStreamRegion::Code => &[10, 5, 1, 3, 0, 1, 11][start..end],
        VwStreamRegion::Tail => &[],
    };
    unsafe {
        *data = bytes.as_ptr();
        *len = bytes.len();
    }
    0
}
extern "C" fn environment(
    user: *mut c_void,
    env: *const VwEnv,
    _: usize,
    _: usize,
    config: *mut VwStreamConfig,
) -> u32 {
    let state = unsafe { state(user) };
    state.env = env;
    unsafe {
        *config = VwStreamConfig {
            batch_bytes: 4,
            max_batches: 1,
        };
    }
    u32::from(state.stop == Stop::Environment)
}
extern "C" fn custom(_: *mut c_void, _: VwStreamRegion, _: usize, _: usize, _: usize) -> u32 {
    0
}
extern "C" fn submit(user: *mut c_void, batch: *mut VwBatch) -> u32 {
    let state = unsafe { state(user) };
    state.batch = batch.cast();
    if state.stop == Stop::Submit {
        return 1;
    }
    unsafe {
        (*batch)
            .0
            .validate_func(0, &mut itasca::EmptyOpVisitor)
            .unwrap();
    }
    0
}
extern "C" fn complete(user: *mut c_void, batch: *mut *mut VwBatch) -> u32 {
    let state = unsafe { state(user) };
    if state.stop == Stop::Complete {
        return 1;
    }
    unsafe {
        *batch = state.batch.cast();
    }
    state.batch = core::ptr::null_mut();
    0
}
extern "C" fn code_end(_: *mut c_void) -> u32 {
    0
}
extern "C" fn tail(user: *mut c_void, tail: *mut VwTail) -> u32 {
    unsafe {
        drop(Box::from_raw(tail));
    }
    u32::from(unsafe { state(user) }.stop == Stop::Tail)
}
extern "C" fn finish(user: *mut c_void, success: bool) {
    let state = unsafe { state(user) };
    state.finishes += 1;
    state.success = success;
    unsafe {
        // Both the borrowed environment and any retained batch remain live.
        if !state.env.is_null() {
            assert_eq!((*state.env).0.func_type_indices.len(), 1);
        }
        if !state.batch.is_null() {
            assert_eq!((*state.batch).0.len(), 1);
            itasca_batch_free(state.batch);
            state.batch = core::ptr::null_mut();
        }
    }
}

#[test]
fn finish_runs_before_environment_release_on_success_and_errors() {
    let visitor = VwStreamVisitor {
        range,
        environment,
        custom,
        submit,
        complete,
        code_end,
        tail,
        finish,
    };
    for stop in [
        Stop::Never,
        Stop::Input,
        Stop::Environment,
        Stop::Submit,
        Stop::Complete,
        Stop::Tail,
    ] {
        let mut state = TestStream {
            stop,
            env: core::ptr::null(),
            batch: core::ptr::null_mut(),
            finishes: 0,
            success: false,
        };
        let mut env = core::ptr::null_mut();
        let result = unsafe {
            itasca_validate_stream(
                &visitor,
                (&mut state as *mut TestStream).cast(),
                7,
                &mut env,
            )
        };
        assert_eq!(state.finishes, 1);
        assert!(state.batch.is_null());
        assert_eq!(state.success, stop == Stop::Never);
        assert_eq!(result.error == VW_OK, stop == Stop::Never);
        if stop == Stop::Never {
            assert_eq!(env as *const VwEnv, state.env);
            unsafe {
                drop(Box::from_raw(env));
            }
        } else {
            assert!(env.is_null());
        }
    }
}
