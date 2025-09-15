// SPDX-FileCopyrightText: 2025 Julian Scheffers <julian@scheffers.net>
// SPDX-FileType: SOURCE
// SPDX-License-Identifier: MIT

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
pub static mut ASID_BITS: u8 = 0;
/// Number of paging levels.
pub static mut PAGING_LEVELS: u8 = 0;
/// Higher half direct map address.
pub static mut HHDM_VADDR: usize = 0;
/// Higher half direct map offset (paddr -> vaddr).
pub static mut HHDM_OFFSET: usize = 0;

/// Read a PTE without any fencing or flushing.
unsafe fn read_pte(pgtable_ppn: PPN, index: VPN) -> PackedPTE {
    let pte_vaddr = unsafe { HHDM_OFFSET }
        + pgtable_ppn * config::PAGE_SIZE as usize
        + index * size_of::<PackedPTE>();
    unsafe { *(pte_vaddr as *const PackedPTE) }
}

/// Write a PTE without any fencing or flushing.
unsafe fn write_pte(pgtable_ppn: PPN, index: VPN, pte: PackedPTE) {
    let pte_vaddr = unsafe { HHDM_OFFSET }
        + pgtable_ppn * config::PAGE_SIZE as usize
        + index * size_of::<PackedPTE>();
    unsafe { *(pte_vaddr as *mut PackedPTE) = pte }
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
                    ppn: orig.ppn + (i << (new_level as usize * BITS_PER_LEVEL)),
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
pub unsafe fn map(mut pgtable_ppn: PPN, target_level: u8, vpn: VPN, new_pte: PTE) -> EResult<()> {
    debug_assert!(!new_pte.leaf);
    let vaddr_bits =
        unsafe { PAGING_LEVELS as usize * BITS_PER_LEVEL } + config::PAGE_SIZE.ilog2() as usize;

    // Descend the page table to the target level.
    for level in (target_level + 1..unsafe { PAGING_LEVELS }).rev() {
        let index = vpn >> (vaddr_bits - (level + 1) as usize * BITS_PER_LEVEL);
        let pte = unsafe { read_pte(pgtable_ppn, index) }.unpack(level);

        pgtable_ppn = if !pte.valid {
            // Create a new level of page table.
            if new_pte.is_null() {
                // Unless the new PTE is null.
                return Ok(());
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
            ppn
        } else {
            pte.ppn
        };
    }

    // Write new PTE.
    let index = vpn >> (vaddr_bits - BITS_PER_LEVEL);
    unsafe {
        write_pte(pgtable_ppn, index, new_pte.pack());
    }

    Ok(())
}

/// Walk down the page table and read the target vaddr's PTE.
/// Returns [`None`] if the PTE doesn't exist, [`Some`] if it does (regardless of validity).
pub unsafe fn walk(mut pgtable_ppn: PPN, vpn: VPN) -> Option<PTE> {
    let vaddr_bits =
        unsafe { PAGING_LEVELS as usize * BITS_PER_LEVEL } + config::PAGE_SIZE.ilog2() as usize;
    let mut pte;

    // Descend the page until a leaf is found.
    for level in (0..unsafe { PAGING_LEVELS }).rev() {
        let index = vpn >> (vaddr_bits - (level + 1) as usize * BITS_PER_LEVEL);
        pte = unsafe { read_pte(pgtable_ppn, index) }.unpack(level);

        if !pte.valid && level > 0 {
            return None;
        } else if pte.valid && !pte.leaf {
            pgtable_ppn = pte.ppn;
        } else {
            return Some(pte);
        }
    }

    unreachable!("Valid non-leaf PTE at level 0");
}
