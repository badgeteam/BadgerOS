use core::ffi::{c_char, c_void};

include!("../../build/bindings.rs");

unsafe extern "C" {
    pub fn malloc(_: usize) -> *mut c_void;
    pub fn calloc(_: usize, _: usize) -> *mut c_void;
    pub fn realloc(_: *mut c_void, _: usize) -> *mut c_void;
    pub fn memset(dest: *mut c_void, val: u8, size: usize);
    pub fn memcpy(dest: *mut c_void, src: *const c_void, size: usize);
    pub fn strlen(cstr: *const c_char) -> usize;
}
