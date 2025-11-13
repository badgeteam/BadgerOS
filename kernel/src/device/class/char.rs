// SPDX-FileCopyrightText: 2025 Julian Scheffers <julian@scheffers.net>
// SPDX-FileType: SOURCE
// SPDX-License-Identifier: MIT

use alloc::boxed::Box;

use crate::{
    bindings::error::{EResult, Errno},
    device::{BaseDriver, Device},
};

pub struct CharSpecialty {
    pub(super) driver: Option<Box<dyn CharDriver>>,
}

/// Character device driver functions.
pub trait CharDriver: BaseDriver {
    /// Read bytes from the device.
    fn read(&self, device: &Device, rdata: &mut [u8]) -> EResult<usize>;
    /// Write bytes to the device.
    fn write(&self, device: &Device, wdata: &[u8]) -> EResult<usize>;
}

impl Device {
    /// Read bytes from the device.
    pub fn char_read(&self, rdata: &mut [u8]) -> EResult<usize> {
        let guard = self.state.lock_shared();
        let chardev = guard.specialty.as_char().ok_or(Errno::EBADF)?;
        let driver = chardev.driver.as_deref().ok_or(Errno::EAGAIN)?;
        driver.read(self, rdata)
    }

    /// Write bytes to the device.
    pub fn char_write(&self, wdata: &[u8]) -> EResult<usize> {
        let guard = self.state.lock_shared();
        let chardev = guard.specialty.as_char().ok_or(Errno::EBADF)?;
        let driver = chardev.driver.as_deref().ok_or(Errno::EAGAIN)?;
        driver.write(self, wdata)
    }
}
