use alloc::boxed::Box;

use crate::{
    bindings::{
        device::{self, BaseDriver, Device, DeviceInfoView, class::char::CharDriver},
        error::EResult,
        raw::driver_char_t,
    },
    char_driver_struct,
};

struct DevZero {}

impl DevZero {
    pub fn new(_device: Device) -> EResult<Box<Self>> {
        Ok(Box::new(Self {}))
    }
}

impl BaseDriver for DevZero {}

impl CharDriver for DevZero {
    fn read(&self, _buf: &mut [u8]) -> EResult<usize> {
        _buf.fill(0);
        Ok(_buf.len())
    }

    fn write(&self, _buf: &[u8]) -> EResult<usize> {
        Ok(_buf.len())
    }
}

fn match_dummy(_info: DeviceInfoView<'_>) -> bool {
    false
}

pub(super) static DEV_ZERO_DRIVER: driver_char_t =
    char_driver_struct!(DevZero, match_dummy, DevZero::new);

pub(super) fn add_driver() {
    device::add_driver(&DEV_ZERO_DRIVER.base).unwrap();
}
