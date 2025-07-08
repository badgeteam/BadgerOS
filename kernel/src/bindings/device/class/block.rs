use core::ffi::c_void;

use crate::bindings::{
    device::{AbstractDevice, BaseDriver},
    error::{EResult, Errno},
    raw::{self, device_block_t},
};

/// Specialization for block devices.
pub type BlockDevice = AbstractDevice<device_block_t>;
impl BlockDevice {
    /// Log-base 2 of device block size.
    pub fn block_size_exp(&self) -> u8 {
        unsafe { (*self.inner.as_ptr()).block_size_exp }
    }
    /// Number of blocks.
    pub fn block_count(&self) -> u64 {
        unsafe { (*self.inner.as_ptr()).block_count }
    }
    /// Device DMA alignment requirement; no more than block size.
    pub fn dma_align(&self) -> usize {
        unsafe { (*self.inner.as_ptr()).dma_align }
    }
    /// Native erased byte value.
    pub fn erase_value(&self) -> u8 {
        unsafe { (*self.inner.as_ptr()).erase_value }
    }
    /// Fast read; do not cache read data, only write data; requires byte read access.
    /// If `false`, all accesses use entire blocks.
    pub fn no_read_cache(&self) -> bool {
        unsafe { (*self.inner.as_ptr()).no_read_cache }
    }
    /// Do not cache at all; requires byte write/erase access.
    /// If `false`, all accesses use entire blocks.
    pub fn no_cache(&self) -> bool {
        unsafe { (*self.inner.as_ptr()).no_cache }
    }

    /// Write device blocks.
    /// The alignment for DMA is handled by this function.
    pub fn write_blocks(&self, start: u64, data: &[u8]) -> EResult<()> {
        let blksize_exp = self.block_size_exp();
        if data.len() as u64 & ((1u64 << blksize_exp) - 1) != 0 {
            return Err(Errno::EINVAL);
        }
        Errno::check(unsafe {
            raw::device_block_write_blocks(
                self.as_raw_ptr(),
                start,
                data.len() as u64 >> blksize_exp,
                data.as_ptr() as *const c_void,
            )
        })
    }

    /// Read device blocks.
    /// The alignment for DMA is handled by this function.
    pub fn read_blocks(&self, start: u64, data: &mut [u8]) -> EResult<()> {
        let blksize_exp = self.block_size_exp();
        if data.len() as u64 & ((1u64 << blksize_exp) - 1) != 0 {
            return Err(Errno::EINVAL);
        }
        Errno::check(unsafe {
            raw::device_block_read_blocks(
                self.as_raw_ptr(),
                start,
                data.len() as u64 >> blksize_exp,
                data.as_mut_ptr() as *mut c_void,
            )
        })
    }

    /// Erase blocks.
    pub fn erase_blocks(&self, start: u64, count: u64) -> EResult<()> {
        Errno::check(unsafe { raw::device_block_erase_blocks(self.as_raw_ptr(), start, count) })
    }

    /// Write block device bytes.
    /// The alignment for DMA is handled by this function.
    pub fn write_bytes(&self, offset: u64, data: &[u8]) -> EResult<()> {
        Errno::check(unsafe {
            raw::device_block_write_bytes(
                self.as_raw_ptr(),
                offset,
                data.len() as u64,
                data.as_ptr() as *const c_void,
            )
        })
    }

    /// Read block device bytes.
    /// The alignment for DMA is handled by this function.
    pub fn read_bytes(&self, offset: u64, data: &mut [u8]) -> EResult<()> {
        Errno::check(unsafe {
            raw::device_block_read_bytes(
                self.as_raw_ptr(),
                offset,
                data.len() as u64,
                data.as_mut_ptr() as *mut c_void,
            )
        })
    }

    /// Erase block device bytes.
    pub fn erase_bytes(&self, offset: u64, size: u64) -> EResult<()> {
        Errno::check(unsafe { raw::device_block_erase_bytes(self.as_raw_ptr(), offset, size) })
    }

    /// Apply all pending changes.
    /// If `flush` is `true`, will remove the cache entries.
    pub fn sync_all(&self, flush: bool) -> EResult<()> {
        Errno::check(unsafe { raw::device_block_sync_all(self.as_raw_ptr(), flush) })
    }

    /// Apply pending changes in a range of blocks.
    /// If `flush` is `true`, will remove the cache entries.
    pub fn sync_blocks(&self, start: u64, count: u64, flush: bool) -> EResult<()> {
        Errno::check(unsafe {
            raw::device_block_sync_blocks(self.as_raw_ptr(), start, count, flush)
        })
    }

    /// Apply pending changes in a range of bytes.
    /// If `flush` is `true`, will remove the cache entries.
    pub fn sync_bytes(&self, offset: u64, size: u64, flush: bool) -> EResult<()> {
        Errno::check(unsafe {
            raw::device_block_sync_bytes(self.as_raw_ptr(), offset, size, flush)
        })
    }
}

/// Block device driver functions.
pub trait BlockDriver: BaseDriver {
    /// Write device blocks.
    /// The caller must ensure that `data` is aligned at least as much as needed for DMA.
    fn write_blocks(&self, start: u64, count: u64, data: &[u8]) -> EResult<()>;
    /// Read device blocks.
    /// The caller must ensure that `data` is aligned at least as much as needed for DMA.
    fn read_blocks(&self, start: u64, count: u64, data: &mut [u8]) -> EResult<()>;
    /// Test whether a single block is erased with native erase value.
    fn is_block_erased(&self, start: u64) -> EResult<bool>;
    /// Erase blocks.
    fn erase_blocks(&self, start: u64, count: u64) -> EResult<()>;
    /// [optional] Write device bytes.
    fn write_bytes(&self, _offset: u64, _data: &[u8]) -> EResult<()> {
        Err(Errno::ENOTSUP)
    }
    /// [optional] Read device bytes.
    fn read_bytes(&self, _offset: u64, _data: &mut [u8]) -> EResult<()> {
        Err(Errno::ENOTSUP)
    }
    /// [optional] Erase bytes.
    fn erase_bytes(&self, _offset: u64, _count: u64) -> EResult<()> {
        Err(Errno::ENOTSUP)
    }
}

/// Helper macro for filling in block driver fields.
#[macro_export]
macro_rules! block_driver_struct {
    ($type: ty, $match_: expr, $add: expr) => {{
        use crate::bindings::{device::class::block::*, error::*, raw::*};
        use ::core::{
            ffi::c_void,
            ptr::{slice_from_raw_parts, slice_from_raw_parts_mut},
        };
        driver_block_t {
            base: crate::abstract_driver_struct! {
                $type,
                dev_class_t_DEV_CLASS_BLOCK,
                $match_,
                $add
            },
            write_blocks: {
                unsafe extern "C" fn write_blocks_wrapper(
                    device: *mut device_block_t,
                    start: u64,
                    count: u64,
                    data: *const c_void,
                ) -> errno_t {
                    let ptr = unsafe { &mut *((*device).base.cookie as *mut $type) };
                    Errno::extract(ptr.write_blocks(start, count, unsafe {
                        &*slice_from_raw_parts(
                            data as *const u8,
                            (count as usize) << (*device).block_size_exp as usize,
                        )
                    }))
                }
                Some(write_blocks_wrapper)
            },
            read_blocks: {
                unsafe extern "C" fn read_blocks_wrapper(
                    device: *mut device_block_t,
                    start: u64,
                    count: u64,
                    data: *mut c_void,
                ) -> errno_t {
                    let ptr = unsafe { &mut *((*device).base.cookie as *mut $type) };
                    Errno::extract(ptr.read_blocks(start, count, unsafe {
                        &mut *slice_from_raw_parts_mut(
                            data as *mut u8,
                            (count as usize) << (*device).block_size_exp as usize,
                        )
                    }))
                }
                Some(read_blocks_wrapper)
            },
            is_block_erased: {
                unsafe extern "C" fn is_block_erased_wrapper(
                    device: *mut device_block_t,
                    block: u64,
                ) -> errno_t {
                    let ptr = unsafe { &mut *((*device).base.cookie as *mut $type) };
                    Errno::extract_bool(ptr.is_block_erased(block))
                }
                Some(is_block_erased_wrapper)
            },
            erase_blocks: {
                unsafe extern "C" fn erase_blocks_wrapper(
                    device: *mut device_block_t,
                    start: u64,
                    count: u64,
                ) -> errno_t {
                    let ptr = unsafe { &mut *((*device).base.cookie as *mut $type) };
                    Errno::extract(ptr.erase_blocks(start, count))
                }
                Some(erase_blocks_wrapper)
            },
            write_bytes: {
                unsafe extern "C" fn write_bytes_wrapper(
                    device: *mut device_block_t,
                    offset: u64,
                    len: u64,
                    data: *const c_void,
                ) -> errno_t {
                    let ptr = unsafe { &mut *((*device).base.cookie as *mut $type) };
                    Errno::extract(ptr.write_bytes(offset, unsafe {
                        &*slice_from_raw_parts(data as *const u8, len as usize)
                    }))
                }
                Some(write_bytes_wrapper)
            },
            read_bytes: {
                unsafe extern "C" fn read_bytes_wrapper(
                    device: *mut device_block_t,
                    offset: u64,
                    len: u64,
                    data: *mut c_void,
                ) -> errno_t {
                    let ptr = unsafe { &mut *((*device).base.cookie as *mut $type) };
                    Errno::extract(ptr.read_bytes(offset, unsafe {
                        &mut *slice_from_raw_parts_mut(data as *mut u8, len as usize)
                    }))
                }
                Some(read_bytes_wrapper)
            },
            erase_bytes: {
                unsafe extern "C" fn erase_bytes_wrapper(
                    device: *mut device_block_t,
                    offset: u64,
                    len: u64,
                ) -> errno_t {
                    let ptr = unsafe { &mut *((*device).base.cookie as *mut $type) };
                    Errno::extract(ptr.erase_bytes(offset, len))
                }
                Some(erase_bytes_wrapper)
            },
        }
    }};
}
