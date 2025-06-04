use core::ffi::c_void;

use crate::bindings::{
    device::{AbstractDevice, BaseDriver},
    error::{EResult, Errno},
    raw::{self, blkdev_erase_t, device_block_t},
};

/// Specialization for block devices.
pub type BlockDevice = AbstractDevice<device_block_t>;
impl BlockDevice {
    // Device block size, must be power of 2.
    pub fn block_size(&self) -> u64 {
        unsafe { (*self.inner.as_ptr()).block_size }
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
        let blksize = self.block_size();
        if data.len() as u64 & (blksize - 1) != 0 {
            return Err(Errno::EINVAL);
        }
        Errno::check(unsafe {
            raw::device_block_write_blocks(
                self.as_raw_ptr(),
                start,
                data.len() as u64 / blksize,
                data.as_ptr() as *const c_void,
            )
        })
    }

    /// Read device blocks.
    /// The alignment for DMA is handled by this function.
    pub fn read_blocks(&self, start: u64, data: &mut [u8]) -> EResult<()> {
        let blksize = self.block_size();
        if data.len() as u64 & (blksize - 1) != 0 {
            return Err(Errno::EINVAL);
        }
        Errno::check(unsafe {
            raw::device_block_read_blocks(
                self.as_raw_ptr(),
                start,
                data.len() as u64 / blksize,
                data.as_mut_ptr() as *mut c_void,
            )
        })
    }

    /// Erase blocks.
    pub fn erase_blocks(&self, start: u64, count: u64, mode: blkdev_erase_t) -> EResult<()> {
        Errno::check(unsafe {
            raw::device_block_erase_blocks(self.as_raw_ptr(), start, count, mode)
        })
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
    pub fn erase_bytes(&self, offset: u64, size: u64, mode: blkdev_erase_t) -> EResult<()> {
        Errno::check(unsafe {
            raw::device_block_erase_bytes(self.as_raw_ptr(), offset, size, mode)
        })
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
    fn is_block_erased(&self, start: u64) -> EResult<()>;
    /// Erase blocks.
    fn erase_blocks(&self, start: u64, count: u64, mode: blkdev_erase_t) -> EResult<()>;
    /// [optional] Write device bytes.
    fn write_bytes(&self, _start: u64, _count: u64, _data: &[u8]) -> EResult<()> {
        Err(Errno::ENOTSUP)
    }
    /// [optional] Read device bytes.
    fn read_bytes(&self, _start: u64, _count: u64, _data: &mut [u8]) -> EResult<()> {
        Err(Errno::ENOTSUP)
    }
    /// [optional] Erase bytes.
    fn erase_bytes(&self, _start: u64, _count: u64, _mode: blkdev_erase_t) -> EResult<()> {
        Err(Errno::ENOTSUP)
    }
}
