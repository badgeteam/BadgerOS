#![no_std]
#![no_main]
#![feature(allocator_api)]
#![feature(formatting_options)]
#![allow(dead_code)]
#![allow(unused_macros)]
#![feature(unsafe_cell_access)]
#![feature(error_generic_member_access)]
#![feature(str_from_raw_parts)]

extern crate alloc;

pub use core::{include, option::Option, result::Result};

#[macro_use]
pub mod bindings;

use core::{alloc::GlobalAlloc, ffi::c_void, panic::PanicInfo};

use alloc::boxed::Box;
use bindings::{
    log::*,
    thread::{Thread, sleep_us},
};

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

#[unsafe(no_mangle)]
pub extern "C" fn rust_test_func() -> i32 {
    logkf!(LogLevel::Debug, "Waiting for 2 seconds");
    let test = Box::new(1u8);
    Thread::new(
        move || {
            sleep_us(2000000);
            logkf!(LogLevel::Debug, "My box contains {}", *test);
            0
        },
        Some("My Rust Thread"),
    )
    .join();
    462
}
