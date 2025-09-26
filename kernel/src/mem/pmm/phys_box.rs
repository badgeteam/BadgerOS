// SPDX-FileCopyrightText: 2025 Julian Scheffers <julian@scheffers.net>
// SPDX-FileType: SOURCE
// SPDX-License-Identifier: MIT

use core::ops::{Deref, DerefMut};

use crate::{
    bindings::error::EResult,
    config::{self, PAGE_SIZE},
    mem,
};

/// A box for physical RAM allocations.
pub struct PhysBox<T: Sized> {
    paddr: usize,
    vaddr: *mut T,
}
unsafe impl<T: Sized> Send for PhysBox<T> {}
unsafe impl<T: Sized + Sync> Sync for PhysBox<T> {}

impl<T: Sized> PhysBox<T> {
    /// Try to allocate some page-aligned physical memory and map it.
    pub unsafe fn try_new(io: bool, nc: bool) -> EResult<Self> {
        unsafe {
            let order = mem::pmm::size_to_order(size_of::<T>());
            let aligned_size = mem::pmm::order_to_size(order);
            let ppn = mem::pmm::page_alloc(order, mem::pmm::PageUsage::KernelAnon)?;
            let paddr = ppn * config::PAGE_SIZE as usize;
            let flags = mem::vmm::flags::RW
                + mem::vmm::flags::A
                + mem::vmm::flags::D
                + io as u32 * mem::vmm::flags::IO
                + nc as u32 * mem::vmm::flags::NC;
            let res = mem::vmm::map_k(aligned_size, ppn, flags);
            if let Err(e) = &res {
                mem::pmm::page_free(ppn, order);
                return Err(*e);
            }
            let vaddr = (res.unwrap() * PAGE_SIZE as usize) as *mut T;
            core::ptr::write_bytes(vaddr as *mut u8, 0, aligned_size);

            Ok(Self { paddr, vaddr })
        }
    }
    /// Get the underlying physical address.
    pub fn paddr(&self) -> usize {
        self.paddr
    }
    /// Leak the underlying physical memory and virtual memory mapping.
    pub fn leak(self) -> (usize, *mut T) {
        let tmp = (self.paddr, self.vaddr);
        core::mem::forget(self);
        tmp
    }
}

impl<T: Sized> Deref for PhysBox<T> {
    type Target = T;

    fn deref(&self) -> &Self::Target {
        unsafe { &*self.vaddr }
    }
}

impl<T: Sized> DerefMut for PhysBox<T> {
    fn deref_mut(&mut self) -> &mut Self::Target {
        unsafe { &mut *self.vaddr }
    }
}

impl<T: Sized> Drop for PhysBox<T> {
    fn drop(&mut self) {
        unsafe {
            let order = mem::pmm::size_to_order(size_of::<T>());
            let aligned_size = mem::pmm::order_to_size(order);
            mem::vmm::unmap_k(
                self.vaddr as usize / config::PAGE_SIZE as usize,
                aligned_size / config::PAGE_SIZE as usize,
            )
            .unwrap();
            mem::pmm::page_free(self.paddr / PAGE_SIZE as usize, order);
        }
    }
}
