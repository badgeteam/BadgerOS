use alloc::boxed::Box;

use crate::bindings::device::class::block::BlockDevice;

/// Specifies some type of media a filesystem can be mounted on.
pub enum MediaType {
    Block(BlockDevice),
    Ram(Box<[u8]>),
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
