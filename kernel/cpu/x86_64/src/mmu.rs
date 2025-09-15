// SPDX-FileCopyrightText: 2025 Julian Scheffers <julian@scheffers.net>
// SPDX-FileType: SOURCE
// SPDX-License-Identifier: MIT

use core::arch::asm;

use crate::mem::{
    pmm::PPN,
    vmm::mmu::{ASID_BITS, PAGING_LEVELS, PTE},
};

#[derive(Debug, Clone, Copy)]
/// Data type that can store a packed page table entry.
pub struct PackedPTE(usize);

impl PackedPTE {
    /// An invalid PTE with no special data in it.
    pub const INVALID: PackedPTE = PackedPTE(0);

    /// Unpack this PTE.
    pub fn unpack(self, level: u8) -> PTE {
        todo!();
    }
}

impl PTE {
    /// Pack this PTE.
    pub fn pack(self) -> PackedPTE {
        todo!();
    }
}

/// Maximum possible value of ASID.
pub const ASID_MAX: usize = 0xffff;

/// Number of virtual address bits per page table level.
pub const BITS_PER_LEVEL: usize = 9;

/// Initialize and detect capabilities of the MMU, given the constructed page table.
pub unsafe fn init(root_ppn: PPN) {
    todo!();
}

#[inline(always)]
/// Switch page table and address space ID.
pub unsafe fn set_page_table(root_ppn: PPN, asid: usize) {
    todo!();
}

#[inline(always)]
/// Perform a fence of virtual memory.
pub fn vmem_fence(vaddr: Option<usize>, asid: Option<usize>) {
    todo!();
}
