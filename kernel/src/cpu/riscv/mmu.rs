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

    #[cfg(target_arch = "riscv32")]
    /// Unpack this PTE.
    pub fn unpack(self, level: u8) -> PTE {
        PTE {
            ppn: (self.0 >> 10) % (1usize << 57),
            flags: (self.0 & 0b111_1110) as u32,
            valid: self.0 & 1 != 0,
            level,
        }
    }

    #[cfg(target_arch = "riscv64")]
    /// Unpack this PTE.
    pub fn unpack(self, level: u8) -> PTE {
        PTE {
            ppn: (self.0 >> 10) % (1usize << 57),
            flags: ((self.0 & 0b111_1110) + (((self.0 >> 61) & 0b11) << 8)) as u32,
            valid: self.0 & 1 != 0,
            leaf: self.0 & 0b1110 == 0,
            level,
        }
    }
}

impl PTE {
    #[cfg(target_arch = "riscv32")]
    /// Pack this PTE.
    pub fn pack(self) -> PackedPTE {
        PackedPTE(
            (self.ppn << 10) as usize + (self.flags & 0b111_1110) as usize + self.valid as usize,
        )
    }

    #[cfg(target_arch = "riscv64")]
    /// Pack this PTE.
    pub fn pack(self) -> PackedPTE {
        let pbmt = if unsafe { HAS_PBMT } {
            ((self.flags as usize >> 8) & 3) << 61
        } else {
            0
        };
        PackedPTE(
            pbmt + (self.ppn << 10) as usize
                + (self.flags & 0b111_1110) as usize
                + self.valid as usize,
        )
    }
}

#[cfg(target_arch = "riscv32")]
/// Maximum possible value of ASID.
pub const ASID_MAX: usize = 0x1ff;
#[cfg(target_arch = "riscv64")]
/// Maximum possible value of ASID.
pub const ASID_MAX: usize = 0xffff;

#[cfg(target_arch = "riscv32")]
/// Number of virtual address bits per page table level.
pub const BITS_PER_LEVEL: usize = 10;
#[cfg(target_arch = "riscv64")]
/// Number of virtual address bits per page table level.
pub const BITS_PER_LEVEL: usize = 9;

/// Whether Svpbmt is supported.
static mut HAS_PBMT: bool = false;

/// Initialize and detect capabilities of the MMU, given the constructed page table.
pub unsafe fn init(root_ppn: PPN) {
    unsafe {
        // Set the kernel page table with the maximum ASID to detect how many ASID bits are available.
        set_page_table(root_ppn, ASID_MAX);
        let asid = read_asid();
        ASID_BITS = asid.trailing_ones() as u8;

        // Set kernel page table with ASID 0 this time (which is reserved for the kernel itself).
        set_page_table(root_ppn, 0);

        // Virtual memory fence to ensure any new things in the kernel page table become visible.
        vmem_fence(None, None);

        // TODO: Detect PBMT once noexc code is supported in Rust.
    }
}

#[inline(always)]
/// Switch page table and address space ID.
pub unsafe fn set_page_table(root_ppn: PPN, asid: usize) {
    #[cfg(target_arch = "riscv32")]
    let new_val = root_ppn + (asid << 22) + (1 << 31);
    #[cfg(target_arch = "riscv64")]
    let new_val = root_ppn + (asid << 44) + (unsafe { PAGING_LEVELS as usize - 3 + 8 } << 60);
    unsafe { asm!("csrw satp, {new_val}", new_val = in(reg) new_val) };
}

#[inline(always)]
/// Read the current ASID out.
fn read_asid() -> usize {
    let val = 0usize;
    unsafe { asm!("csrr {val}, satp", val = out(reg) _) };
    #[cfg(target_arch = "riscv32")]
    let res = (val >> 22) & 0x1ff;
    #[cfg(target_arch = "riscv64")]
    let res = (val >> 44) & 0xffff;
    res
}

#[inline(always)]
/// Perform a fence of virtual memory.
pub fn vmem_fence(vaddr: Option<usize>, asid: Option<usize>) {
    unsafe {
        match (vaddr, asid) {
            (None, None) => asm!("sfence.vma"),
            (None, Some(asid)) => asm!("sfence.vma x0, {asid}", asid = in(reg) asid),
            (Some(vaddr), None) => asm!("sfence.vma {vaddr}, x0", vaddr = in(reg) vaddr),
            (Some(vaddr), Some(asid)) => {
                asm!("sfence.vma {vaddr}, {asid}", vaddr = in(reg) vaddr, asid = in(reg) asid)
            }
        }
    }
}
