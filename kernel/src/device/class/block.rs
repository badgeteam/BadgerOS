// SPDX-FileCopyrightText: 2025 Julian Scheffers <julian@scheffers.net>
// SPDX-FileType: SOURCE
// SPDX-License-Identifier: MIT

use alloc::boxed::Box;

use crate::{
    bindings::{
        error::{EResult, Errno},
        mutex::Mutex,
    },
    device::{BaseDriver, Device},
};

pub struct BlockSpecialty {
    pub(super) driver: Option<Box<dyn BlockDriver>>,
    /// What number this block device is within its node name family.
    /// For example, for /dev/sata0 this would be 0.
    pub node_number: u32,
    /// Cached geometry, if any.
    geometry: Mutex<Option<BlockGeometry>>,
}

/// Block device geometry and attributes.
#[derive(Clone, Copy, Debug)]
pub struct BlockGeometry {
    /// Log2 of the block size in bytes.
    pub block_size_exp: u8,
    /// Total number of blocks on the device.
    pub block_count: u64,
    /// Supports byte-level read access.
    /// Advertising this disables read caching in higher layers.
    pub byte_readable: bool,
    /// Supports byte-level write access; must also be byte readable.
    /// Advertising this disables all caching in higher layers.
    pub byte_writable: bool,
    /// Log2 of DMA alignment requirement in bytes.
    pub dma_align_exp: u8,
}

/// Block device driver functions.
pub trait BlockDriver: BaseDriver {
    /// Get device node name.
    /// For example, "sata" implies disks like /dev/sata0 and partitions like /dev/sata0p1.
    /// Multiple drivers may share the same node name.
    fn node_name(&self) -> &'static str;

    /// Get block device attributes and geometry.
    fn get_geometry(&self, device: &Device) -> EResult<BlockGeometry>;

    /// Read blocks from the device.
    fn read_blocks(&self, device: &Device, start_block: u64, rdata: &mut [u8]) -> EResult<()>;

    /// Write blocks to the device.
    fn write_blocks(&self, device: &Device, start_block: u64, wdata: &[u8]) -> EResult<()>;

    /// Erase blocks on the device.
    fn erase_blocks(&self, device: &Device, start_block: u64, num_blocks: u64) -> EResult<()>;

    /// Sync disk's write cache to physical media.
    fn sync_blocks(&self, device: &Device, start_block: u64, num_blocks: u64) -> EResult<()>;

    /// Read bytes from the device.
    /// Only called if the device advertises byte-level access.
    fn read_bytes(&self, _device: &Device, _offset: u64, _rdata: &mut [u8]) -> EResult<()> {
        Err(Errno::ENOSYS)
    }

    /// Write bytes to the device.
    /// Only called if the device advertises byte-level access.
    fn write_bytes(&self, _device: &Device, _offset: u64, _wdata: &[u8]) -> EResult<()> {
        Err(Errno::ENOSYS)
    }

    /// Erase bytes on the device.
    /// Only called if the device advertises byte-level access.
    fn erase_bytes(&self, _device: &Device, _offset: u64, _num_bytes: u64) -> EResult<()> {
        Err(Errno::ENOSYS)
    }

    /// Sync byte-level device's write cache to physical media.
    /// Only called if the device advertises byte-level access.
    fn sync_bytes(&self, _device: &Device, _offset: u64, _num_bytes: u64) -> EResult<()> {
        Err(Errno::ENOSYS)
    }
}

impl Device {
    /// Get block device attributes and geometry.
    pub fn block_get_geometry(&self) -> EResult<BlockGeometry> {
        let guard = self.state.lock_shared();
        let blockdev = guard.specialty.as_block().ok_or(Errno::EBADF)?;
        let driver = blockdev.driver.as_deref().ok_or(Errno::EAGAIN)?;

        if let Some(geometry) = *blockdev.geometry.lock() {
            return Ok(geometry);
        }

        let mut geometry_guard = blockdev.geometry.lock();
        if let Some(geometry) = *geometry_guard {
            return Ok(geometry);
        }

        let geometry = driver.get_geometry(self)?;
        *geometry_guard = Some(geometry);

        Ok(geometry)
    }

    /// Read blocks from the device.
    pub fn block_read_blocks(&self, start_block: u64, rdata: &mut [u8]) -> EResult<()> {
        let block_size_exp = self.block_get_geometry()?.block_size_exp;
        if rdata.len() % (1 << block_size_exp) != 0 {
            return Err(Errno::EINVAL);
        }
        self.block_read_bytes(start_block << block_size_exp, rdata)
    }

    /// Write blocks to the device.
    pub fn block_write_blocks(&self, start_block: u64, wdata: &[u8]) -> EResult<()> {
        let block_size_exp = self.block_get_geometry()?.block_size_exp;
        if wdata.len() % (1 << block_size_exp) != 0 {
            return Err(Errno::EINVAL);
        }
        self.block_write_bytes(start_block << block_size_exp, wdata)
    }

    /// Erase blocks on the device.
    pub fn block_erase_blocks(&self, start_block: u64, num_blocks: u64) -> EResult<()> {
        let guard = self.state.lock_shared();
        let blockdev = guard.specialty.as_block().ok_or(Errno::EBADF)?;
        let driver = blockdev.driver.as_deref().ok_or(Errno::EAGAIN)?;

        todo!("Cached block erase")
    }

    /// Sync disk's write cache to physical media.
    pub fn block_sync(&self, start_block: u64, num_blocks: u64) -> EResult<()> {
        let guard = self.state.lock_shared();
        let blockdev = guard.specialty.as_block().ok_or(Errno::EBADF)?;
        let driver = blockdev.driver.as_deref().ok_or(Errno::EAGAIN)?;

        todo!("Cached block sync")
    }

    /// Read bytes from the device.
    pub fn block_read_bytes(&self, offset: u64, rdata: &mut [u8]) -> EResult<()> {
        let guard = self.state.lock_shared();
        let blockdev = guard.specialty.as_block().ok_or(Errno::EBADF)?;
        let driver = blockdev.driver.as_deref().ok_or(Errno::EAGAIN)?;

        if self.block_get_geometry()?.byte_readable {
            driver.read_bytes(self, offset, rdata)
        } else {
            todo!("Cached byte read")
        }
    }

    /// Write bytes to the device.
    pub fn block_write_bytes(&self, offset: u64, wdata: &[u8]) -> EResult<()> {
        let guard = self.state.lock_shared();
        let blockdev = guard.specialty.as_block().ok_or(Errno::EBADF)?;
        let driver = blockdev.driver.as_deref().ok_or(Errno::EAGAIN)?;

        if self.block_get_geometry()?.byte_writable {
            driver.write_bytes(self, offset, wdata)
        } else {
            todo!("Cached byte write")
        }
    }

    /// Erase bytes on the device.
    pub fn block_erase_bytes(&self, offset: u64, num_bytes: u64) -> EResult<()> {
        let guard = self.state.lock_shared();
        let blockdev = guard.specialty.as_block().ok_or(Errno::EBADF)?;
        let driver = blockdev.driver.as_deref().ok_or(Errno::EAGAIN)?;
        let geometry = self.block_get_geometry()?;
        if geometry.byte_writable {
            driver.erase_bytes(self, offset, num_bytes)
        } else {
            let block_size = 1u64 << geometry.block_size_exp;
            if num_bytes == 0 {
                return Ok(());
            }
            let end = offset.checked_add(num_bytes).ok_or(Errno::EINVAL)?;

            // first block fully inside range
            let first_block = (offset + block_size - 1) / block_size;
            // one-past-last block fully inside range
            let end_block_excl = end / block_size;

            if end_block_excl <= first_block {
                return Ok(());
            }

            let num_blocks = end_block_excl - first_block;
            driver.erase_blocks(self, first_block, num_blocks)
        }
    }

    /// Sync byte-level device's write cache to physical media.
    pub fn block_sync_bytes(&self, offset: u64, num_bytes: u64) -> EResult<()> {
        let guard = self.state.lock_shared();
        let blockdev = guard.specialty.as_block().ok_or(Errno::EBADF)?;
        let driver = blockdev.driver.as_deref().ok_or(Errno::EAGAIN)?;

        if self.block_get_geometry()?.byte_writable {
            driver.sync_bytes(self, offset, num_bytes)
        } else {
            todo!("Cached byte sync")
        }
    }

    /// Sync all cached data to physical media.
    pub fn block_sync_all(&self) -> EResult<()> {
        let guard = self.state.lock_shared();
        let blockdev = guard.specialty.as_block().ok_or(Errno::EBADF)?;
        let driver = blockdev.driver.as_deref().ok_or(Errno::EAGAIN)?;

        todo!("Cached full device sync")
    }
}
