// SPDX-FileCopyrightText: 2025 Julian Scheffers <julian@scheffers.net>
// SPDX-FileType: SOURCE
// SPDX-License-Identifier: MIT

//! This a special buddy allocator that allows for reference counting blocks.
//! Already allocated blocks can be split further if partially freed (not the entire block has the reference dropped).
//! The smallest block size is [`crate::config::PAGE_SIZE`].
//! This allocator creates pages of overhead as necessary to allocate metadata.

use core::{
    cell::UnsafeCell,
    mem::ManuallyDrop,
    sync::atomic::{AtomicU32, AtomicUsize},
};

use crate::{
    bindings::error::EResult,
    config,
    mem::{
        pmm::{AtomicPPN, AtomicPaddr, PPN, Paddr, PhysAlloc},
        vmm::{self, VPN},
    },
};

/// A temporary physical memory allocator that uses either the global allocator, or reserves pages linearly before the metadata is mapped.
/// This is used as the memory source for mapping this physical allocator into the page table.
/// Doesn't need to free because this would only happen if allocation fails, which would cause the [`PhysBuddy`] not to be created.
/// Allocates pages at increasing physical addresses.
struct PhysBuddyTempAlloc {
    /// Physical page number of the first block.
    base: PPN,
    /// How many times the first allocated page is referenced.
    /// The page table builder should only (possibly) clone for the first allocated page,
    /// because it will be for the highest level in the kernel page table, which is mirrored to user page tables.
    first_refcount: UnsafeCell<u32>,
    /// Number of pages allocated so far.
    usage: UnsafeCell<PPN>,
    /// Total number of pages allowed to be allocated.
    total: PPN,
}

impl PhysAlloc for PhysBuddyTempAlloc {
    fn page_alloc(&self, order: u8) -> Option<PPN> {
        debug_assert!(order == 0);
        // SAFETY: Because of how this is intended to be used, only one thread will call any function in here.
        let usage = unsafe { self.usage.as_mut_unchecked() };
        if *usage + 1 < self.total {
            let ppn = self.base + *usage;
            *usage += 1;
            Some(ppn)
        } else {
            None
        }
    }

    unsafe fn page_clone(&self, base: PPN, size: PPN) {
        debug_assert!(
            base == self.base && *unsafe { self.usage.as_ref_unchecked() } == 0 && size == 1
        );
        // SAFETY: Because of how this is intended to be used, only one thread will call any function in here.
        // In addition, we know the caller to be the page table builder, which will only possibly clone the very first page it allocates.
        *unsafe { self.first_refcount.as_mut_unchecked() } += 1;
    }

    unsafe fn page_drop(&self, _base: PPN, _size: PPN) {
        // This doesn't leak memory because pages would only be dropped if building the page table fails,
        // which would lead to the real allocator not being created.
    }
}

#[repr(C)]
/// Metadata helper type for [`PhysBuddy`].
struct PhysBuddyMetadata {
    /// The slot reserved for the allocator itself.
    alloc: PhysBuddy,
    /// The amount of free blocks per order.
    available: [AtomicPPN; 64],
    /// The start of the per-block metadata.
    block_metadata: [AtomicU32; 0],
}

#[repr(C)]
/// A physical memory buddy allocator.
///
/// The metadata is comprised of a contiguous array of 32-bit atomic flags:
/// - \[31:26] The block's order
/// - \[25]    Is part of a larger block
/// - \[24:0]  If \[25]: How many entries to go back for the main metadata entry, else: The reference count.
///
/// In addition, a metadata slot is locked if set to 0xffff_ffff.
pub struct PhysBuddy {
    /// Physical page number of the first block.
    base: PPN,
    /// Number of available data pages.
    page_count: PPN,
    /// How many pages of overhead are used to store the metadata.
    overhead: PPN,
    /// Raw pointer to the mapped metadata.
    /// Reserves extra space near the beginning so this struct can optionally be placed there.
    metadata: *const PhysBuddyMetadata,
    /// This may not be dropped implicitly.
    nodrop: ManuallyDrop<()>,
}

impl PhysBuddy {
    /// Initialize a physical memory allocator, if the region is large enough.
    pub unsafe fn new(base: PPN, len: PPN) -> EResult<Self> {
        debug_assert!(size_of::<PhysBuddyMetadata>() == size_of::<[AtomicPPN; 64]>() + size_of::<PhysBuddy>());
        
        // Calculate overhead.
        let byte_overhead =
            len * size_of::<AtomicU32>() + size_of::<[AtomicPPN; 64]>() + size_of::<PhysBuddy>();
        let page_overhead = byte_overhead.div_ceil(config::PAGE_SIZE as usize) as PPN;

        // Map the metadata into memory.
        let tmp_alloc = PhysBuddyTempAlloc {
            base,
            first_refcount: UnsafeCell::new(1),
            usage: UnsafeCell::new(0),
            total: len - page_overhead,
        };
        let metadata_vpn =
            unsafe { vmm::map_k(page_overhead as VPN, base, vmm::flags::RW, &tmp_alloc) }?;
        let metadata = (metadata_vpn * config::PAGE_SIZE as usize) as *const ();

        let alloc = Self {
            base,
            page_count: len - page_overhead,
            overhead: page_overhead,
            metadata,
            nodrop: ManuallyDrop::new(()),
        };
        
        // Get the metadata pointers.
        let available = unsafe {&*(alloc.metadata as usize + )}
        
        // Initialize the metadata.

        Ok(alloc)
    }
}
