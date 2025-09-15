// SPDX-FileCopyrightText: 2025 Julian Scheffers <julian@scheffers.net>
// SPDX-FileType: SOURCE
// SPDX-License-Identifier: MIT

use core::ops::Range;

use crate::{
    bindings::error::EResult,
    config,
    cpu::mmu::{BITS_PER_LEVEL, PackedPTE},
    mem::pmm::{PPN, page_alloc},
};

use super::*;

#[derive(Debug, Clone, Copy)]
/// Generic representation of a page table entry.
pub struct PTE {
    /// Physical page number that this PTE points to.
    pub ppn: PPN,
    /// Page protection flags, see [`super::vmm::flags`].
    pub flags: u32,
    /// At what level of the page table this PTE is stored.
    pub level: u8,
    /// Whether this PTE is valid.
    pub valid: bool,
    /// Whether this is a leaf PTE.
    pub leaf: bool,
}

impl PTE {
    pub fn is_null(&self) -> bool {
        self.ppn == 0 && self.flags == 0 && !self.valid
    }
}

/// How many bits of address space ID are available.
pub static mut ASID_BITS: u32 = 0;
/// Number of paging levels.
pub static mut PAGING_LEVELS: u32 = 0;

/// Read a PTE without any fencing or flushing.
pub(super) unsafe fn read_pte(pgtable_ppn: PPN, index: usize) -> PackedPTE {
    let pte_vaddr = unsafe { HHDM_OFFSET }
        + pgtable_ppn * config::PAGE_SIZE as usize
        + index * size_of::<PackedPTE>();
    unsafe { *(pte_vaddr as *const PackedPTE) }
}

/// Write a PTE without any fencing or flushing.
pub(super) unsafe fn write_pte(pgtable_ppn: PPN, index: usize, pte: PackedPTE) {
    let pte_vaddr = unsafe { HHDM_OFFSET }
        + pgtable_ppn * config::PAGE_SIZE as usize
        + index * size_of::<PackedPTE>();
    unsafe { *(pte_vaddr as *mut PackedPTE) = pte }
}

/// Get the index in the given page table level for the given virtual address.
fn get_vpn_index(vpn: VPN, level: u8) -> usize {
    let vaddr_bits = unsafe { PAGING_LEVELS * BITS_PER_LEVEL } + config::PAGE_SIZE.ilog2();
    (vpn >> (vaddr_bits - (level + 1) as u32 * BITS_PER_LEVEL)) % (1usize << BITS_PER_LEVEL)
}

/// Try to allocate a new page table page.
fn alloc_pgtable_page() -> EResult<PPN> {
    let ppn = unsafe { page_alloc(0) }?;
    for i in 0..1usize << BITS_PER_LEVEL {
        unsafe { write_pte(ppn, i, PackedPTE::INVALID) };
    }
    Ok(ppn)
}

/// Try to split a page table leaf node.
fn split_pgtable_leaf(orig: PTE, new_level: u8) -> EResult<PPN> {
    debug_assert!(orig.leaf && orig.valid);
    let ppn = unsafe { page_alloc(0) }?;
    for i in 0..1usize << BITS_PER_LEVEL {
        unsafe {
            write_pte(
                ppn,
                i,
                PTE {
                    ppn: orig.ppn + (i << (new_level as u32 * BITS_PER_LEVEL)),
                    level: new_level,
                    ..orig
                }
                .pack(),
            )
        };
    }
    Ok(ppn)
}

/// Create a new mapping in the page table.
/// Returns whether the page table root was touched.
pub unsafe fn map(mut pgtable_ppn: PPN, vpn: VPN, new_pte: PTE) -> EResult<bool> {
    debug_assert!(new_pte.leaf);
    let mut root_touched = false;

    // Descend the page table to the target level.
    for level in (new_pte.level + 1..unsafe { PAGING_LEVELS as u8 }).rev() {
        let index = get_vpn_index(vpn, level as u8);
        let pte = unsafe { read_pte(pgtable_ppn, index) }.unpack(level);

        pgtable_ppn = if !pte.valid {
            // Create a new level of page table.
            if new_pte.is_null() {
                // Unless the new PTE is null.
                return Ok(false);
            }
            let ppn = alloc_pgtable_page()?;
            unsafe {
                write_pte(
                    pgtable_ppn,
                    index,
                    PTE {
                        ppn,
                        flags: new_pte.flags & flags::G,
                        valid: true,
                        leaf: false,
                        level,
                    }
                    .pack(),
                )
            };
            root_touched = true;
            ppn
        } else if pte.leaf {
            // Split up the leaf node.
            let ppn = split_pgtable_leaf(pte, level - 1)?;
            unsafe {
                write_pte(
                    pgtable_ppn,
                    index,
                    PTE {
                        ppn,
                        flags: new_pte.flags & flags::G,
                        valid: true,
                        leaf: false,
                        level,
                    }
                    .pack(),
                )
            };
            root_touched = true;
            ppn
        } else {
            pte.ppn
        };
    }

    // Write new PTE.
    let index = get_vpn_index(vpn, new_pte.level);
    unsafe {
        write_pte(pgtable_ppn, index, new_pte.pack());
    }

    Ok(root_touched)
}

/// Walk down the page table and read the target vaddr's PTE.
pub unsafe fn walk(mut pgtable_ppn: PPN, vpn: VPN) -> PTE {
    let mut pte;

    // Descend the page until a leaf is found.
    for level in (0..unsafe { PAGING_LEVELS }).rev() {
        let index = get_vpn_index(vpn, level as u8);
        pte = unsafe { read_pte(pgtable_ppn, index) }.unpack(level as u8);

        if !pte.valid && level > 0 {
            return pte;
        } else if pte.valid && !pte.leaf {
            pgtable_ppn = pte.ppn;
        } else {
            return pte;
        }
    }

    unreachable!("Valid non-leaf PTE at level 0");
}

/// Determine whether an address is canonical.
pub fn is_canon_addr(addr: usize) -> bool {
    let addr = addr as isize;
    let exp = usize::BITS - PAGE_SIZE.ilog2() - BITS_PER_LEVEL * unsafe { PAGING_LEVELS };
    let canon_addr = (addr << exp) >> exp;
    canon_addr == addr
}

/// Determine whether an address is a canonical kernel address.
pub fn is_canon_kernel_addr(addr: usize) -> bool {
    is_canon_addr(addr) && (addr as isize) < 0
}

/// Determine whether an address is a canonical user address.
pub fn is_canon_user_addr(addr: usize) -> bool {
    is_canon_addr(addr) && (addr as isize) >= 0
}

/// Determine whether an address is canonical.
pub fn is_canon_range(range: Range<usize>) -> bool {
    is_canon_addr(range.start) && (range.len() == 0 || is_canon_addr(range.end - 1))
}

/// Determine whether an address is a canonical kernel address.
pub fn is_canon_kernel_range(range: Range<usize>) -> bool {
    is_canon_kernel_addr(range.start) && (range.len() == 0 || is_canon_kernel_addr(range.end - 1))
}

/// Determine whether an address is a canonical user address.
pub fn is_canon_user_range(range: Range<usize>) -> bool {
    is_canon_user_addr(range.start) && (range.len() == 0 || is_canon_user_addr(range.end - 1))
}

/// Get the size of a "half" of the canonical ranges.
pub fn canon_half_size() -> usize {
    (PAGE_SIZE as usize) << (BITS_PER_LEVEL * unsafe { PAGING_LEVELS })
}

/// Get the start of the higher half.
pub fn higher_half_vaddr() -> usize {
    canon_half_size().wrapping_neg()
}
