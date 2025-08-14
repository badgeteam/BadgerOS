use core::cell::UnsafeCell;

use alloc::boxed::Box;

use crate::bindings::{
    device::class::block::BlockDevice,
    error::{EResult, Errno},
};

/// Specifies some type of media a filesystem can be mounted on.
pub enum MediaType {
    Block(BlockDevice),
    Ram(UnsafeCell<Box<[u8]>>),
}

/// Specifies a partition to mount a filesystem on.
pub struct Media {
    /// Partition byte offset.
    pub offset: u64,
    /// Partition byte size.
    pub size: u64,
    /// Partition underlying storage.
    pub storage: MediaType,
}
unsafe impl Sync for Media {}

impl Media {
    /// Write data to the media.
    pub fn write(&self, offset: u64, data: &[u8]) -> EResult<()> {
        let offset = offset.checked_add(self.offset).ok_or(Errno::EIO)?;
        let end = offset.checked_add(data.len() as u64).ok_or(Errno::EIO)?;
        if end > self.size {
            return Err(Errno::EIO);
        }
        match &self.storage {
            MediaType::Block(block_device) => {
                block_device.write_bytes(offset, data)?;
            }
            MediaType::Ram(ram) => {
                let buffer = unsafe { ram.as_mut_unchecked() };
                buffer[offset as usize..offset as usize + data.len()].copy_from_slice(data);
            }
        }
        Ok(())
    }

    /// Read data from the media.
    pub fn read(&self, offset: u64, data: &mut [u8]) -> EResult<()> {
        let offset = offset.checked_add(self.offset).ok_or(Errno::EIO)?;
        let end = offset.checked_add(data.len() as u64).ok_or(Errno::EIO)?;
        if end > self.size {
            return Err(Errno::EIO);
        }
        match &self.storage {
            MediaType::Block(block_device) => {
                block_device.read_bytes(offset, data)?;
            }
            MediaType::Ram(ram) => {
                let buffer = unsafe { ram.as_mut_unchecked() };
                data.copy_from_slice(&buffer[offset as usize..offset as usize + data.len()]);
            }
        }
        Ok(())
    }

    /// Sync a region of the media.
    pub fn sync(&self, offset: u64, len: u64) -> EResult<()> {
        let offset = offset.checked_add(self.offset).ok_or(Errno::EIO)?;
        let end = offset.checked_add(len).ok_or(Errno::EIO)?;
        if end > self.size {
            return Err(Errno::EIO);
        }
        match &self.storage {
            MediaType::Block(block_device) => {
                block_device.sync_bytes(offset, len, false)?;
            }
            MediaType::Ram(_) => {
                // RAM doesn't need explicit sync.
            }
        }
        Ok(())
    }
}
