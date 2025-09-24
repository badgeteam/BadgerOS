use core::sync::atomic::{AtomicU32, AtomicUsize};

use crate::bindings::{
    self,
    error::{EResult, Errno},
};

pub mod phys_box;

/// Kinds of usage for pages of memory.
#[repr(u8)]
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum PageUsage {
    /// Part of a page table.
    PageTable,
    /// Contains cached data.
    Cache,
    /// Part of a mmap'ed file.
    Mmap,
    /// Anonymous user memory.
    UserAnon,
    /// Anonymous kernel memory.
    KernelAnon,
}

pub mod pgflags {
    /// Page struct is locked for modifications.
    pub const LOCKED: u32 = 0x0000_0001;
    /// Page may be swapped to disk.
    pub const SWAPPABLE: u32 = 0x0000_0002;
    /// Bitmask: Page usage.
    pub const USAGE_MASK: u32 = 0xff00_0000;
    /// Bit exponent: Page usage.
    pub const USAGE_EXP: u32 = 24;
}

/// Physical memory page metadata.
#[repr(C)]
pub struct Page {
    /// Page refcount, typically 1 for kernel pages.
    pub refcount: AtomicU32,
    /// Page flags and usage kind.
    pub flags: AtomicU32,
    // TODO: Pointer to structure that exposes where it's mapped in user virtual memory.
    // Kernel virtual mappings need not be tracked because they are not swappable.
}

/// Unsigned integer that can store a physical page number.
pub type AtomicPPN = AtomicUsize;
/// Unsigned integer that can store a physical page number.
pub type PPN = usize;

/// Total physical pages.
pub static TOTAL_PAGES: AtomicPPN = AtomicPPN::new(0);
/// Physical pages that are used by the kernel.
pub static KERNEL_PAGES: AtomicPPN = AtomicPPN::new(0);
/// Physical pages that are caches.
pub static CACHE_PAGES: AtomicPPN = AtomicPPN::new(0);
/// Physical pages used in use.
pub static USED_PAGES: AtomicPPN = AtomicPPN::new(0);

/// Allocate pages of physical memory.
pub unsafe fn page_alloc(count: usize) -> EResult<PPN> {
    let tmp = unsafe { bindings::raw::phys_page_alloc(count, false) };
    if tmp == 0 {
        return Err(Errno::ENOMEM);
    }
    Ok(tmp)
}

/// Free pages of physical memory.
pub unsafe fn page_free(ppn: PPN, count: usize) {
    // TODO: Current PMM doesn't support this sized model very well.
    unsafe { bindings::raw::phys_page_free(ppn) }
}

/// Get the [`Page`] struct for some physical page number.
/// Manipulating the data within the struct is unsafe.
pub unsafe fn page_struct(ppn: PPN) -> Option<&'static Page> {
    todo!()
}
