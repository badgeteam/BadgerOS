#![no_std]
#![no_main]
// #![feature(lang_items)]

use core::panic::PanicInfo;

#[panic_handler]
pub fn badgeros_rust_panic(_info: &PanicInfo) -> ! {
    loop {}
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_test_func() -> i32 {
    462
}
