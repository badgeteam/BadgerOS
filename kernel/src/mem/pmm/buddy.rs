// SPDX-FileCopyrightText: 2025 Julian Scheffers <julian@scheffers.net>
// SPDX-FileType: SOURCE
// SPDX-License-Identifier: MIT

//! This a special buddy allocator that allows for reference counting blocks.
//! Already allocated blocks can be split further if partially freed (not the entire block has the reference dropped).
//! The smallest block size is [`crate::config::PAGE_SIZE`].
//! This allocator creates pages of overhead as necessary to allocate metadata.

use core::{
    cell::UnsafeCell,
    mem::{ManuallyDrop, offset_of},
    ptr::slice_from_raw_parts_mut,
    sync::atomic::{AtomicUsize, Ordering, fence},
};

use crate::{
    bindings::error::EResult,
    config,
    mem::{
        pmm::{PPN, PhysAlloc},
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

    unsafe fn page_free(&self, _base: PPN) {}
}

/// Highest order block used by this allocator.
const MAX_ORDER: u8 = 4;

#[repr(C)]
/// Per-block metadata for [`PhysBuddy`].
struct BlockMetadata {
    /// What order of block this is.
    order: u8,
    /// Whether this block is part of a larger block.
    is_subblock: bool,
    /// Next free block of the same order.
    next: usize,
    /// Previous free block of the same order.
    prev: usize,
}

#[repr(C)]
/// Per-order metadata for [`PhysBuddy`].
struct OrderMetadata {
    /// First block of this order.
    first: AtomicUsize,
}

#[repr(C)]
/// Metadata helper type for [`PhysBuddy`].
struct PhysBuddyMetadata {
    /// The slot reserved for the allocator itself.
    alloc: PhysBuddy,
    /// The amount of free blocks per order.
    available: [OrderMetadata; MAX_ORDER as usize],
    /// The start of the per-block metadata.
    block_metadata: [BlockMetadata; 0],
}

#[repr(C)]
/// A physical memory buddy allocator.
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
        debug_assert!(
            offset_of!(PhysBuddyMetadata, block_metadata)
                == size_of::<[OrderMetadata; MAX_ORDER as usize]>() + size_of::<PhysBuddy>()
        );

        // Calculate overhead.
        let byte_overhead = len * size_of::<BlockMetadata>() + size_of::<PhysBuddyMetadata>();
        let page_overhead = byte_overhead.div_ceil(config::PAGE_SIZE as usize) as PPN;

        // Map the metadata into memory.
        let tmp_alloc = PhysBuddyTempAlloc {
            base,
            usage: UnsafeCell::new(0),
            total: len - page_overhead,
        };
        let metadata_vpn =
            unsafe { vmm::map_k(page_overhead as VPN, base, vmm::flags::RW, &tmp_alloc) }?;
        let metadata = (metadata_vpn * config::PAGE_SIZE as usize) as *mut PhysBuddyMetadata;

        let alloc = Self {
            base,
            page_count: len - page_overhead,
            overhead: page_overhead,
            metadata,
            nodrop: ManuallyDrop::new(()),
        };

        // Get metadata pointers.
        let available = unsafe { &mut (*metadata).available };
        let block_metadata =
            unsafe { &mut *slice_from_raw_parts_mut(&raw mut (*metadata).block_metadata[0], len) };

        // Initialize the metadata.
        available.fill_with(|| OrderMetadata {
            first: AtomicUsize::new(usize::MAX),
        });
        block_metadata.fill_with(|| BlockMetadata {
            order: 0,
            is_subblock: false,
            next: 0,
            prev: 0,
        });
        let tmp_usage = *unsafe { tmp_alloc.usage.as_ref_unchecked() };
        if tmp_usage != 0 {
            block_metadata[0].refcount.store(
                *unsafe { tmp_alloc.first_refcount.as_ref_unchecked() },
                Ordering::Relaxed,
            );
        }
        for i in 1..tmp_usage {
            block_metadata[i].refcount.store(1, Ordering::Relaxed);
        }

        // Do initial coalescing of blocks.
        let mut block = tmp_usage;
        while block < alloc.page_count {
            // Calculate maximum order of block that can be made.
            let order = ((block + alloc.base).trailing_zeros())
                .min((alloc.page_count - block).trailing_zeros()) as u8;

            // Set block metadata.
            block_metadata[block].order = order;
            for subblock in block + 1..block + 1 << order {
                block_metadata[subblock].order = order;
                block_metadata[subblock].is_subblock = true;
            }

            block = block + (1 << order);
        }

        fence(Ordering::Release);
        Ok(alloc)
    }
}
