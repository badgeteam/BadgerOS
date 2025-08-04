use core::ffi::c_void;

use alloc::vec::Vec;

use crate::bindings::{
    device::{AbstractDevice, BaseDriver, Device, DeviceFilters},
    error::{EResult, Errno},
    raw::{self, dev_class_t_DEV_CLASS_CHAR, device_char_t},
};

/// Specialization for character devices.
pub type CharDevice = AbstractDevice<device_char_t>;
impl CharDevice {
    /// Get a list of devices using a filter.
    pub fn filter(filters: DeviceFilters) -> EResult<Vec<CharDevice>> {
        unsafe {
            Device::filter_impl::<device_char_t, CharDevice, true>(
                filters,
                dev_class_t_DEV_CLASS_CHAR,
            )
        }
    }

    /// Read bytes from the device.
    pub fn read(&self, rdata: &mut [u8]) -> EResult<usize> {
        Errno::check_usize(unsafe {
            raw::device_char_read(
                self.as_raw_ptr(),
                rdata.as_ptr() as *mut c_void,
                rdata.len(),
            )
        })
    }

    /// Write bytes to the device.
    pub fn write(&self, wdata: &[u8]) -> EResult<usize> {
        Errno::check_usize(unsafe {
            raw::device_char_write(
                self.as_raw_ptr(),
                wdata.as_ptr() as *const c_void,
                wdata.len(),
            )
        })
    }
}

/// Character device driver functions.
pub trait CharDriver: BaseDriver {
    /// Read bytes from the device.
    fn read(&self, rdata: &mut [u8]) -> EResult<usize>;
    /// Write bytes to the device.
    fn write(&self, wdata: &[u8]) -> EResult<usize>;
}

/// Helper macro for filling in character driver fields.
#[macro_export]
macro_rules! char_driver_struct {
    ($type: ty, $match_: expr, $add: expr) => {{
        use crate::bindings::{device::class::char::*, error::*, raw::*};
        use ::core::{
            ffi::c_void,
            ptr::{slice_from_raw_parts, slice_from_raw_parts_mut},
        };
        driver_char_t {
            base: crate::abstract_driver_struct! {
                $type,
                dev_class_t_DEV_CLASS_BLOCK,
                $match_,
                $add
            },
            write: {
                unsafe extern "C" fn write_wrapper(
                    device: *mut device_char_t,
                    wdata: *const c_void,
                    wdata_len: usize,
                ) -> errno_size_t {
                    let ptr = unsafe { &mut *((*device).base.cookie as *mut $type) };
                    Errno::extract_usize(
                        ptr.write(unsafe { &*slice_from_raw_parts(wdata as *const u8, wdata_len) }),
                    )
                }
                Some(write_wrapper)
            },
            read: {
                unsafe extern "C" fn read_wrapper(
                    device: *mut device_char_t,
                    rdata: *mut c_void,
                    rdata_len: usize,
                ) -> errno_size_t {
                    let ptr = unsafe { &mut *((*device).base.cookie as *mut $type) };
                    Errno::extract_usize(ptr.read(unsafe {
                        &mut *slice_from_raw_parts_mut(rdata as *mut u8, rdata_len)
                    }))
                }
                Some(read_wrapper)
            },
        }
    }};
}
