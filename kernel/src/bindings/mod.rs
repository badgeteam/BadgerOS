use raw::timestamp_us_t;

#[allow(non_upper_case_globals)]
#[allow(non_camel_case_types)]
#[allow(non_snake_case)]
#[allow(unused)]
#[allow(unsafe_op_in_unsafe_fn)]
#[allow(non_upper_case_globals)]
pub mod raw;

pub mod device;

pub mod dlist;
#[macro_use]
pub mod log;
#[macro_use]
pub mod error;
pub mod irq;
pub mod mutex;
pub mod pmm;
pub mod process;
pub mod semaphore;
pub mod thread;
pub mod usercopy;

pub fn time_us() -> timestamp_us_t {
    unsafe { raw::time_us() }
}
