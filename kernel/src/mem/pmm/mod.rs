// SPDX-FileCopyrightText: 2025 Julian Scheffers <julian@scheffers.net>
// SPDX-FileType: SOURCE
// SPDX-License-Identifier: MIT

use core::sync::atomic::AtomicUsize;

use crate::bindings;

// pub mod buddy;

/// Unsigned integer that can store a physical address.
pub type AtomicPaddr = AtomicUsize;
/// Unsigned integer that can store a physical page number.
pub type AtomicPPN = AtomicPaddr;
/// Unsigned integer that can store a physical address.
pub type Paddr = usize;
/// Unsigned integer that can store a physical page number.
pub type PPN = Paddr;

/// API for physical memory allocators.
/// The physical memory allocators cannot completely guarantee safety;
/// it is up to the caller to guarantee no double-free, use-after-free or clone of free memory.
pub trait PhysAlloc {
    /// Try to allocate a block of the desired size `1 << order`.
    /// Returns the physical page number of the start of the block.
    fn page_alloc(&self, order: u8) -> Option<PPN>;

    /// Decrease the reference count of a range of physical memory.
    /// The caller must ensure that `base` points to the start of a valid allocation,
    /// and note that this entire allocation is freed at once.
    unsafe fn page_free(&self, base: PPN);
}

/// The global page allocator.
pub static GLOBAL_PHYS_ALLOC: GlobalPhysAlloc = GlobalPhysAlloc { a: () };

pub struct GlobalPhysAlloc {
    /// Dummy field that forbids construction of this allocator externally.
    a: (),
}

impl PhysAlloc for GlobalPhysAlloc {
    fn page_alloc(&self, order: u8) -> Option<PPN> {
        let tmp = unsafe { bindings::raw::phys_page_alloc(1usize << order, false) };
        if tmp == 0 {
            return None;
        }
        Some(tmp)
    }

    unsafe fn page_free(&self, base: PPN) {
        unsafe { bindings::raw::phys_page_free(base) };
    }
}
