use core::{
    ffi::c_void,
    ops::{Deref, DerefMut},
};

use alloc::alloc::AllocError;

use crate::{
    bindings::raw::{MEMPROTECT_FLAG_IO, MEMPROTECT_FLAG_NC, MEMPROTECT_FLAG_RW},
    config,
};

use super::raw;

/// A box for physical RAM allocations.
pub struct PhysBox<T: Sized> {
    paddr: usize,
    vaddr: *mut T,
}
unsafe impl<T: Sized> Send for PhysBox<T> {}
unsafe impl<T: Sized + Sync> Sync for PhysBox<T> {}

impl<T: Sized> PhysBox<T> {
    /// Try to allocate some page-aligned physical memory and map it.
    pub unsafe fn try_new(io: bool, nc: bool) -> Result<Self, AllocError> {
        unsafe {
            let page_count = size_of::<T>().div_ceil(config::PAGE_SIZE as usize);
            let aligned_size = page_count * config::PAGE_SIZE as usize;
            let ppn = raw::phys_page_alloc(page_count, false);
            if ppn == 0 {
                return Err(AllocError {});
            }
            let paddr = ppn * config::PAGE_SIZE as usize;
            let vaddr = raw::memprotect_alloc_vaddr(aligned_size);
            if vaddr == 0 {
                raw::phys_page_free(ppn);
                return Err(AllocError {});
            }
            let flags = MEMPROTECT_FLAG_RW
                + io as u32 * MEMPROTECT_FLAG_IO
                + nc as u32 * MEMPROTECT_FLAG_NC;
            let res = raw::memprotect_k(vaddr, paddr, aligned_size, flags);
            raw::memprotect_commit(&raw mut raw::mpu_global_ctx);
            if !res {
                raw::memprotect_free_vaddr(vaddr);
                raw::phys_page_free(ppn);
                return Err(AllocError {});
            }

            raw::memset(vaddr as *mut c_void, 0, size_of::<T>());

            Ok(Self {
                paddr,
                vaddr: vaddr as *mut T,
            })
        }
    }
    /// Get the underlying physical address.
    pub fn paddr(&self) -> usize {
        self.paddr
    }
    /// Leak the underlying physical memory and virtual memory mapping.
    pub fn leak(self) -> (usize, *mut T) {
        (self.paddr, self.vaddr)
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
            let page_count = size_of::<T>().div_ceil(config::PAGE_SIZE as usize);
            let aligned_size = page_count * config::PAGE_SIZE as usize;
            raw::memprotect_k(self.vaddr as usize, self.paddr, aligned_size, 0);
            raw::memprotect_commit(&raw mut raw::mpu_global_ctx);
            raw::phys_page_free(self.paddr / config::PAGE_SIZE as usize);
        }
    }
}
