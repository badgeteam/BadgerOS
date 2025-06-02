use core::{
    ffi::{c_char, c_int, c_void},
    str::FromStr,
};

use alloc::{boxed::Box, ffi::CString};

use super::{
    error::ErrnoError,
    raw::{self, SCHED_PRIO_NORMAL, tid_t, timestamp_us_t},
};

pub struct Thread {
    handle: tid_t,
}

unsafe extern "C" fn rust_thread_trampoline(arg: *mut c_void) -> i32 {
    unsafe { Box::from_raw(arg as *mut Box<dyn FnOnce() -> i32>)() }
}

impl Thread {
    pub fn from_id(id: tid_t) -> Self {
        Self { handle: id }
    }
    pub fn try_new<T: FnOnce() -> i32 + Send + 'static>(
        code: T,
        name: Option<&str>,
    ) -> Result<Self, ErrnoError> {
        unsafe {
            let closure: Box<dyn FnOnce() -> i32> = Box::new(code);
            let arg = Box::into_raw(Box::new(closure));
            let name = name.map(|f| CString::from_str(f).unwrap());
            let tid = raw::thread_new_kernel(
                name.map(|f| f.as_ptr()).unwrap_or(0 as *const c_char),
                Some(rust_thread_trampoline),
                arg as *mut c_void,
                SCHED_PRIO_NORMAL as c_int,
            );
            if tid <= 0 {
                drop(Box::from_raw(arg));
                Err(ErrnoError {
                    errno: core::mem::transmute(-tid),
                })
            } else {
                raw::thread_resume(tid);
                Ok(Self { handle: tid })
            }
        }
    }
    pub fn new<T: FnOnce() -> i32 + Send + 'static>(code: T, name: Option<&str>) -> Self {
        Self::try_new(code, name).unwrap()
    }
    pub fn join(self) -> i32 {
        unsafe { raw::thread_join(self.handle) as i32 }
    }
    pub fn detach(self) {
        unsafe { raw::thread_detach(self.handle) };
    }

    pub fn sleep_us(delay: timestamp_us_t) {
        unsafe { raw::thread_sleep(delay) }
    }

    pub unsafe fn exit(code: i32) -> ! {
        unsafe { raw::thread_exit(code) }
    }
}
