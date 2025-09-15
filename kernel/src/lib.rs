#![no_std]
#![no_main]
#![feature(allocator_api)]
#![feature(formatting_options)]
#![allow(dead_code)]
#![allow(unused_macros)]
#![feature(unsafe_cell_access)]
#![feature(error_generic_member_access)]
#![feature(str_from_raw_parts)]
#![feature(negative_impls)]
#![feature(ptr_metadata)]
#![feature(box_vec_non_null)]
#![feature(vec_into_raw_parts)]
#![feature(try_with_capacity)]
#![feature(str_from_utf16_endian)]
#![feature(ascii_char)]
#![feature(atomic_try_update)]
#![feature(try_blocks)]
#![feature(likely_unlikely)]
#![feature(map_try_insert)]
#![feature(iterator_try_collect)]
#![feature(int_lowest_highest_one)]

#[macro_use]
extern crate alloc;
extern crate chrono;

#[macro_use]
pub mod bindings;
pub mod badgelib;
pub mod config;
pub mod device;
pub mod filesystem;
pub mod kparam;
pub mod mem;
pub mod util;

#[cfg(any(target_arch = "riscv32", target_arch = "riscv64"))]
#[path = "../cpu/riscv/src/mod.rs"]
pub mod cpu;
#[cfg(target_arch = "x86_64")]
#[path = "../cpu/x86_64/src/mod.rs"]
pub mod cpu;

use bindings::log::*;
use core::{alloc::GlobalAlloc, ffi::c_void, ops::Deref, panic::PanicInfo};

#[derive(Clone, Copy)]
pub struct ReadOnly<T> {
    inner: T,
}

impl<T> ReadOnly<T> {
    pub fn new<'a>(thing: &'a mut T) -> &'a mut ReadOnly<T> {
        unsafe { &mut *(thing as *mut T as *mut ReadOnly<T>) }
    }
}

impl<T> Deref for ReadOnly<T> {
    type Target = T;

    fn deref(&self) -> &Self::Target {
        &self.inner
    }
}

#[global_allocator]
pub static BADGEROS_RUST_MALLOC: BadgerOSMalloc = BadgerOSMalloc {};

pub struct BadgerOSMalloc {}

unsafe impl GlobalAlloc for BadgerOSMalloc {
    unsafe fn alloc(&self, layout: core::alloc::Layout) -> *mut u8 {
        unsafe { bindings::raw::malloc(layout.pad_to_align().size()) as *mut u8 }
    }

    unsafe fn dealloc(&self, ptr: *mut u8, _: core::alloc::Layout) {
        unsafe { bindings::raw::free(ptr as *mut c_void) }
    }

    unsafe fn alloc_zeroed(&self, layout: core::alloc::Layout) -> *mut u8 {
        unsafe { bindings::raw::calloc(1, layout.pad_to_align().size()) as *mut u8 }
    }

    unsafe fn realloc(&self, ptr: *mut u8, _: core::alloc::Layout, new_size: usize) -> *mut u8 {
        unsafe { bindings::raw::realloc(ptr as *mut c_void, new_size) as *mut u8 }
    }
}

#[panic_handler]
pub fn badgeros_rust_panic(info: &PanicInfo) -> ! {
    unsafe {
        bindings::raw::claim_panic();
        logk_unlocked(LogLevel::Fatal, "Rust code panicked!");

        if let Some(loc) = info.location() {
            logkf_unlocked!(
                LogLevel::Fatal,
                "At {}:{}:{}",
                loc.file(),
                loc.line(),
                loc.column()
            );
        }

        logkf_unlocked(LogLevel::Fatal, &info.message());

        bindings::raw::panic_abort_unchecked()
    }
}
