use alloc::vec::Vec;
use dev_null::DEV_NULL_DRIVER;
use dev_zero::DEV_ZERO_DRIVER;

use crate::bindings::{
    self,
    device::{Device, DeviceInfo},
};

pub mod ahci;
pub mod dev_null;
pub mod dev_zero;

#[unsafe(no_mangle)]
unsafe extern "C" fn add_rust_builtin_drivers() {
    ahci::add_drivers();
    dev_null::add_driver();
    dev_zero::add_driver();
}

#[unsafe(no_mangle)]
unsafe extern "C" fn device_create_null_zero() {
    let dev_null = Device::add(DeviceInfo {
        parent: None,
        irq_parent: None,
        addrs: Vec::new(),
        phandle: None,
    })
    .unwrap();
    unsafe {
        bindings::raw::device_set_driver(dev_null.as_raw_ptr(), &raw const DEV_NULL_DRIVER.base)
    };

    let dev_zero = Device::add(DeviceInfo {
        parent: None,
        irq_parent: None,
        addrs: Vec::new(),
        phandle: None,
    })
    .unwrap();
    unsafe {
        bindings::raw::device_set_driver(dev_zero.as_raw_ptr(), &raw const DEV_ZERO_DRIVER.base)
    };
}
