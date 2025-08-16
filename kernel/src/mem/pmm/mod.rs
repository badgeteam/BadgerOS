// SPDX-FileCopyrightText: 2025 Julian Scheffers <julian@scheffers.net>
// SPDX-FileType: SOURCE
// SPDX-License-Identifier: MIT

use core::sync::atomic::AtomicUsize;

pub mod buddy;

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

    /// Increase the reference count of a range of physical memory.
    /// # Panics
    /// - If the specified range is not within this allocator;
    /// - Part of the range is currently marked as free;
    /// - The reference count overflows.
    unsafe fn page_clone(&self, base: PPN, size: PPN);

    /// Decrease the reference count of a range of physical memory.
    /// # Panics
    /// - If the specified range is not within this allocator;
    /// - Part of the range is currently marked as free.
    unsafe fn page_drop(&self, base: PPN, size: PPN);
}
