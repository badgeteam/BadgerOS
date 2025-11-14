// SPDX-FileCopyrightText: 2025 Julian Scheffers <julian@scheffers.net>
// SPDX-FileType: SOURCE
// SPDX-License-Identifier: MIT

use core::{
    hint::unreachable_unchecked,
    ops::Range,
    sync::atomic::{AtomicU32, AtomicUsize, Ordering},
};

use crate::{
    badgelib::irq::IrqGuard,
    bindings::{
        error::{EResult, Errno},
        spinlock::Spinlock,
    },
    config,
    mem::vmm,
};

mod c_api;
pub mod phys_box;

/// Kinds of usage for pages of memory.
#[repr(u32)]
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum PageUsage {
    /// Unused page.
    Free = 0,
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
    /// Kernel slabs memory (may be removed in the future).
    KernelSlab,
    /// Dummy entry for unusable page.
    Unusable,
}

pub mod pgflags {
    /// Page struct is locked for modifications.
    pub const LOCKED: u32 = 0x0000_0001;
    /// Page may be swapped to disk.
    pub const SWAPPABLE: u32 = 0x0000_0002;
    /// Bitmask: buddy alloc page order.
    pub const ORDER_MASK: u32 = 0x00fc_0000;
    /// Bit exponent: buddy alloc page order.
    pub const ORDER_EXP: u32 = 18;
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

impl Page {
    /// Get the buddy alloc page order.
    pub fn order(&self) -> u32 {
        (self.flags.load(Ordering::Relaxed) & pgflags::ORDER_MASK) >> pgflags::ORDER_EXP
    }
    /// Set the buddy alloc page order.
    fn set_order(&self, order: u32) {
        self.flags
            .update(Ordering::Relaxed, Ordering::Relaxed, |flags| {
                (flags & !pgflags::ORDER_MASK) | (order << pgflags::ORDER_EXP)
            });
    }
    /// Get the buddy alloc page order.
    pub fn usage(&self) -> PageUsage {
        let usage =
            (self.flags.load(Ordering::Relaxed) & pgflags::USAGE_MASK) >> pgflags::USAGE_EXP;
        unsafe { core::mem::transmute(usage) }
    }
    /// Set the buddy alloc page order.
    fn set_usage(&self, usage: PageUsage) {
        let usage = usage as u32;
        self.flags
            .update(Ordering::Relaxed, Ordering::Relaxed, |flags| {
                (flags & !pgflags::USAGE_MASK) | (usage << pgflags::USAGE_EXP)
            });
    }
}

/// Physical memory freelist link.
#[derive(Clone, Copy)]
struct FreeListLink {
    prev: PPN,
    next: PPN,
}

/// Unsigned integer that can store a physical page number.
pub type AtomicPPN = AtomicUsize;
/// Unsigned integer that can store a physical page number.
pub type PPN = usize;

/// Maximum order that blocks can be coalesced into.
pub const MAX_ORDER: u32 = 64;

/// Total physical pages.
pub static TOTAL_PAGES: AtomicPPN = AtomicPPN::new(0);
/// Physical pages that are used by the kernel.
pub static KERNEL_PAGES: AtomicPPN = AtomicPPN::new(0);
/// Physical pages that are caches (can be reclaimed if low on free pages).
pub static CACHE_PAGES: AtomicPPN = AtomicPPN::new(0);
/// Physical pages that are unused.
pub static FREE_PAGES: AtomicPPN = AtomicPPN::new(0);
/// Physical pages used in use.
pub static USED_PAGES: AtomicPPN = AtomicPPN::new(0);

/// Range of pages covered by the page allocator.
static mut PAGE_RANGE: Range<PPN> = 0..0;
/// Pointer to the page struct array.
static mut PAGE_STRUCTS_PAGE: PPN = 0;
/// Free lists per buddy order.
static FREE_LIST: Spinlock<[PPN; MAX_ORDER as usize]> =
    unsafe { Spinlock::new_static([PPN::MAX; MAX_ORDER as usize]) };

/// Calculates the minimum sized order that will fit this many bytes.
pub const fn size_to_order(byte_size: usize) -> u32 {
    debug_assert!(byte_size > 0);
    let pages = byte_size.div_ceil(config::PAGE_SIZE as usize) as PPN;
    PPN::BITS - (pages - 1).leading_zeros()
}

/// Calculates how many bytes are in a block of a certain order.
pub const fn order_to_size(order: u32) -> usize {
    debug_assert!(order < MAX_ORDER);
    (config::PAGE_SIZE as usize) << order
}

/// Helper function that gets the freelist link for a block, assuming it is free.
unsafe fn free_list_struct(page: PPN) -> *mut FreeListLink {
    (page * config::PAGE_SIZE as usize + unsafe { vmm::HHDM_OFFSET }) as *mut FreeListLink
}

/// Unlink a page from its freelist.
unsafe fn free_list_unlink(block: PPN, list_head: &mut PPN) {
    debug_assert!(unsafe { free_list_contains(block, *list_head) });
    let link = unsafe { *free_list_struct(block) };
    debug_assert!(link.prev != block);
    debug_assert!(link.next != block);

    if link.next != PPN::MAX {
        let next_link = unsafe { &mut *free_list_struct(link.next) };
        next_link.prev = link.prev;
        debug_assert!(next_link.prev != link.next);
        debug_assert!(next_link.next != link.next);
    }

    if link.prev != PPN::MAX {
        let prev_link = unsafe { &mut *free_list_struct(link.prev) };
        prev_link.next = link.next;
        debug_assert!(prev_link.next != link.prev);
        debug_assert!(prev_link.prev != link.prev);
    } else {
        *list_head = link.next;
    }
    debug_assert!(!unsafe { free_list_contains(block, *list_head) });
}

/// Link a page into a freelist.
unsafe fn free_list_link(block: PPN, list_head: &mut PPN) {
    debug_assert!(!unsafe { free_list_contains(block, *list_head) });
    let link = unsafe { &mut *free_list_struct(block) };

    link.next = *list_head;
    link.prev = PPN::MAX;
    if link.next != PPN::MAX {
        let next_link = unsafe { &mut *free_list_struct(link.next) };
        next_link.prev = block;
        debug_assert!(next_link.prev != link.next);
        debug_assert!(next_link.next != link.next);
    }

    *list_head = block;

    debug_assert!(link.prev != block);
    debug_assert!(link.next != block);
    debug_assert!(unsafe { free_list_contains(block, *list_head) });
}

/// Check whether a page is in a certain freelist.
unsafe fn free_list_contains(block: PPN, list_head: PPN) -> bool {
    debug_assert!(unsafe { PAGE_RANGE.start <= block && block < PAGE_RANGE.end });
    let mut cur_node = list_head;
    while cur_node != PPN::MAX {
        debug_assert!(unsafe { PAGE_RANGE.start <= cur_node && cur_node < PAGE_RANGE.end });
        if cur_node == block {
            return true;
        }
        let link = unsafe { *free_list_struct(cur_node) };
        cur_node = link.next;
    }
    false
}

/// Allocate `1 << order` pages of physical memory.
pub unsafe fn page_alloc(order: u32, usage: PageUsage) -> EResult<PPN> {
    debug_assert!(order < MAX_ORDER);
    debug_assert!(usage != PageUsage::Unusable && usage != PageUsage::Free);
    let _noirq = unsafe { IrqGuard::new() };
    let mut free_list = FREE_LIST.lock();

    // Determine order to split at.
    let mut split_order = order;
    while free_list[split_order as usize] == PPN::MAX {
        if split_order >= MAX_ORDER - 1 {
            return Err(Errno::ENOMEM);
        }
        split_order += 1;
    }

    // Split blocks until one of the desired order is created.
    for split_order in (order + 1..split_order + 1).rev() {
        let block = free_list[split_order as usize];
        // Not asserting block order here because it might be temporarily out of date.
        debug_assert!(block == block >> split_order << split_order);
        unsafe { free_list_unlink(block, &mut free_list[split_order as usize]) };
        // Upper half of block gets the order changed because the lower half will be alloc'ed anyway
        for i in 1 << (split_order - 1)..1 << split_order {
            let page_meta = unsafe { &*page_struct(block + i) };
            page_meta.set_order(split_order - 1);
        }
        unsafe {
            // Do not reorder these inserts.
            free_list_link(
                block + (1 << (split_order - 1)),
                &mut free_list[split_order as usize - 1],
            );
            free_list_link(block, &mut free_list[split_order as usize - 1]);
        }
    }

    // Mark block as in use.
    let block = free_list[order as usize];
    if block == PPN::MAX {
        return Err(Errno::ENOMEM);
    }
    unsafe { free_list_unlink(block, &mut free_list[order as usize]) };
    for i in 0..1 << order {
        let page_meta = unsafe { &*page_struct(block + i) };
        page_meta.refcount.store(1, Ordering::Relaxed);
        page_meta
            .flags
            .store((usage as u32) << pgflags::USAGE_EXP, Ordering::Relaxed);
    }

    drop(free_list);

    // Account memory usage.
    FREE_PAGES.fetch_sub(1 << order, Ordering::Relaxed);
    match usage {
        PageUsage::Free | PageUsage::Unusable => unsafe { unreachable_unchecked() },
        PageUsage::Cache => {
            CACHE_PAGES.fetch_add(1 << order, Ordering::Relaxed);
        }
        PageUsage::PageTable | PageUsage::KernelAnon | PageUsage::KernelSlab => {
            KERNEL_PAGES.fetch_add(1 << order, Ordering::Relaxed);
            USED_PAGES.fetch_add(1 << order, Ordering::Relaxed);
        }
        PageUsage::Mmap | PageUsage::UserAnon => {
            USED_PAGES.fetch_add(1 << order, Ordering::Relaxed);
        }
    }

    Ok(block)
}

/// Free pages of physical memory.
pub unsafe fn page_free(block: PPN, order: u32) {
    unsafe {
        debug_assert!(block >> order << order == block);
        let page_meta = page_struct(block);
        let refcount = (*page_meta).refcount.fetch_sub(1, Ordering::Relaxed);
        debug_assert!(refcount == 1);
        mark_one_free(block, order);
    }
}

/// Get the [`Page`] struct for some physical page number.
/// Manipulating the data within the struct is unsafe.
pub fn page_struct(ppn: PPN) -> *mut Page {
    unsafe {
        debug_assert!(PAGE_RANGE.start <= ppn && ppn < PAGE_RANGE.end);
        let vaddr = PAGE_STRUCTS_PAGE * config::PAGE_SIZE as usize
            + vmm::HHDM_OFFSET
            + (ppn - PAGE_RANGE.start) * size_of::<Page>();
        vaddr as *mut Page
    }
}

/// Get the [`Page`] struct for the start of the block that some physical page number lies in.
/// Manipulating the data within the struct is unsafe.
pub fn page_struct_base(ppn: PPN, order: u32) -> *mut Page {
    unsafe {
        let page = page_struct(ppn >> order << order);
        debug_assert!((*page).order() == order);
        return page;
    }
}

/// Mark a single block of arbitrary order as free.
unsafe fn mark_one_free(mut block: PPN, mut order: u32) {
    debug_assert!(unsafe { PAGE_RANGE.start <= block && block < PAGE_RANGE.end });
    debug_assert!(block % (1usize << order) == 0);
    let pages_freed: PPN = 1 << order;
    let _noirq = unsafe { IrqGuard::new() };
    let mut free_list = FREE_LIST.lock();

    // Remove the pages from where they were previously accounted.
    let page_meta = unsafe { &*page_struct(block) };
    match page_meta.usage() {
        PageUsage::Free => unreachable!("Unused page marked as free again"),
        PageUsage::Unusable => (), // Not accounted as being used for something.
        PageUsage::Cache => {
            CACHE_PAGES.fetch_sub(pages_freed, Ordering::Relaxed);
        }
        PageUsage::PageTable | PageUsage::KernelAnon | PageUsage::KernelSlab => {
            KERNEL_PAGES.fetch_sub(pages_freed, Ordering::Relaxed);
            USED_PAGES.fetch_sub(pages_freed, Ordering::Relaxed);
        }
        PageUsage::Mmap | PageUsage::UserAnon => {
            USED_PAGES.fetch_sub(pages_freed, Ordering::Relaxed);
        }
    }

    /// Try to coalesce a block with its buddy.
    fn try_coalesce(block: PPN, order: u32, free_list: &mut [PPN; MAX_ORDER as usize]) -> bool {
        // Determine whether coalescing is possible.
        let buddy = block ^ (1 << order);
        if !unsafe { PAGE_RANGE.start <= buddy && buddy < PAGE_RANGE.end } {
            return false;
        }
        let buddy_page = unsafe { &*page_struct(buddy) };
        if buddy_page.refcount.load(Ordering::Relaxed) > 0 || buddy_page.order() != order {
            return false;
        }
        debug_assert!(buddy_page.usage() == PageUsage::Free);

        // Remove buddy from freelist.
        unsafe {
            free_list_unlink(buddy, &mut free_list[order as usize]);
        }

        true
    }

    // Attempt to coalesce.
    while order < MAX_ORDER && try_coalesce(block, order, &mut free_list) {
        block &= !(1 << order);
        order += 1;
    }

    // Set the pages up for future usage.
    for i in block..block + (1 << order) {
        let page_meta = unsafe { &*page_struct(i) };
        page_meta
            .flags
            .store(order << pgflags::ORDER_EXP, Ordering::Relaxed);
        page_meta.refcount.store(0, Ordering::Relaxed);
    }

    // Insert it into the freelist.
    unsafe { free_list_link(block, &mut free_list[order as usize]) };

    // Account the memory as free.
    FREE_PAGES.fetch_add(pages_freed, Ordering::Relaxed);
}

/// Mark a range of blocks as free.
pub unsafe fn mark_free(mut pages: Range<PPN>) {
    while pages.end > pages.start {
        // Max order of the page depends on physical address and available space.
        let max_order = pages
            .start
            .trailing_zeros()
            .min((pages.end - pages.start).ilog2());
        unsafe { mark_one_free(pages.start, max_order) };
        pages.start += 1 << max_order;
    }
}

/// Initialize the physical memory allocator.
pub unsafe fn init(total: Range<PPN>, early: Range<PPN>) {
    unsafe {
        TOTAL_PAGES.store(total.end - total.start, Ordering::Relaxed);
        PAGE_RANGE = total.clone();
        // How many pages will be used by the page metadata structs.
        let meta_pages = size_of::<Page>().div_ceil(config::PAGE_SIZE as usize) as PPN;
        // There needs to be at least a small amount of available pages to bootstrap MM.
        if early.end - early.start < meta_pages + 64 {
            panic!("Insufficient memory");
        }
        PAGE_STRUCTS_PAGE = early.start;

        // Mark all pages as unusable...
        for page in total {
            let page_struct = page_struct(page);
            *page_struct = core::mem::zeroed();
            (*page_struct).set_usage(PageUsage::Unusable);
            (*page_struct).refcount.store(1, Ordering::Relaxed);
        }

        // ...but mark the early pool as free.
        mark_free(early.start + meta_pages..early.end);
    }
}

pmm_ktest!(PMM_BASIC, unsafe {
    // Allocate one page of a couple orders.
    let mut ppn = [0; 4];
    for order in 0..3 {
        ppn[order] = page_alloc(order as u32, PageUsage::KernelAnon)?;
    }
    // Make sure we didn't get duplicates.
    for x in 1..3 {
        for y in 0..x {
            ktest_expect!(ppn[x], !=, ppn[y], [x, y]);
        }
    }
    // Ensure metadata for alloc'ed blocks is ok.
    for order in 0..3 {
        let page_meta = &*page_struct(ppn[order]);
        ktest_expect!(page_meta.refcount.load(Ordering::Relaxed), 1);
        ktest_assert!(!free_list_contains(ppn[order], FREE_LIST.lock()[order]));
    }
    // Free pages again.
    for order in 0..3 {
        page_free(ppn[order], order as u32);
    }
});
