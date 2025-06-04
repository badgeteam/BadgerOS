#![allow(non_upper_case_globals)]
#![allow(non_camel_case_types)]
#![allow(non_snake_case)]
#![allow(unused)]
#![allow(unsafe_op_in_unsafe_fn)]
#![allow(non_upper_case_globals)]
use core::ffi::{c_char, c_void};

include!("../../build/bindings.rs");

unsafe extern "C" {
    pub fn mem_copy(dest: *mut c_void, src: *const c_void, size: usize);
    pub fn strlen(cstr: *const c_char) -> usize;
}
