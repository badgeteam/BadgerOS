use raw::timestamp_us_t;

pub mod device;

pub mod dlist;
#[macro_use]
pub mod log;
#[macro_use]
pub mod error;
pub mod irq;
pub mod mutex;
pub mod process;
pub mod raw;
pub mod semaphore;
pub mod thread;
pub mod usercopy;

pub fn time_us() -> timestamp_us_t {
    unsafe { raw::time_us() }
}
