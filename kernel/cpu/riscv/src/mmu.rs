// SPDX-FileCopyrightText: 2025 Julian Scheffers <julian@scheffers.net>
// SPDX-FileType: SOURCE
// SPDX-License-Identifier: MIT

use core::arch::asm;

use crate::mem::{
    pmm::PPN,
    vmm::mmu::{ASID_BITS, PAGING_LEVELS, PTE},
};

/// Data type that can store a packed page table entry.
pub type PackedPTE = usize;
/// An invalid PTE with no special data in it.
pub const INVALID_PTE: PackedPTE = 0;

impl PTE {
    #[cfg(target_arch = "riscv32")]
    /// Unpack this PTE.
    pub fn unpack(raw: PackedPTE, level: u8) -> PTE {
        PTE {
            ppn: (raw >> 10) % (1usize << 57),
            flags: (raw & 0b11_1111_1110) as u32,
            valid: raw & 1 != 0,
            leaf: raw & 0b1110 != 0,
            level,
        }
    }

    #[cfg(target_arch = "riscv64")]
    /// Unpack this PTE.
    pub fn unpack(raw: PackedPTE, level: u8) -> PTE {
        PTE {
            ppn: (raw >> 10) % (1usize << 57),
            flags: ((raw & 0b11_1111_1110) + (((raw >> 61) & 0b11) << 10)) as u32,
            valid: raw & 1 != 0,
            leaf: raw & 0b1110 != 0,
            level,
        }
    }

    #[cfg(target_arch = "riscv32")]
    /// Pack this PTE.
    pub fn pack(self) -> PackedPTE {
        (self.ppn << 10) as usize + (self.flags & 0b11_1111_1110) as usize + self.valid as usize
    }

    #[cfg(target_arch = "riscv64")]
    /// Pack this PTE.
    pub fn pack(self) -> PackedPTE {
        let pbmt = if unsafe { HAS_PBMT } {
            ((self.flags as usize >> 10) & 3) << 61
        } else {
            0
        };
        pbmt + (self.ppn << 10) as usize
            + (self.flags & 0b11_1111_1110) as usize
            + self.valid as usize
    }
}

#[cfg(target_arch = "riscv32")]
/// Maximum possible value of ASID.
pub const ASID_MAX: u32 = 0x1ff;
#[cfg(target_arch = "riscv64")]
/// Maximum possible value of ASID.
pub const ASID_MAX: u32 = 0xffff;

#[cfg(target_arch = "riscv32")]
/// Number of virtual address bits per page table level.
pub const BITS_PER_LEVEL: u32 = 10;
#[cfg(target_arch = "riscv64")]
/// Number of virtual address bits per page table level.
pub const BITS_PER_LEVEL: u32 = 9;

/// Heuristic for maximum number of pages to individually invalidate.
pub const INVAL_PAGE_THRESHOLD: usize = 16;

/// Whether Svpbmt is supported.
static mut HAS_PBMT: bool = false;

/// Perform early MMU initialization using the existing page tables (which were created by the bootloader).
pub unsafe fn early_init() {
    #[cfg(target_arch = "riscv32")]
    unsafe {
        PAGING_LEVELS = 2;
    }
    #[cfg(target_arch = "riscv64")]
    unsafe {
        let satp: usize;
        asm!("csrr {r}, satp", r = out(reg) satp);
        let mode = satp >> 60;
        PAGING_LEVELS = mode as u32 - 8 + 3;
    }
}

/// Initialize and detect capabilities of the MMU, given the constructed page table.
pub unsafe fn init(root_ppn: PPN) {
    unsafe {
        // Set the kernel page table with the maximum ASID to detect how many ASID bits are available.
        set_page_table(root_ppn, 0);
        let asid = read_asid();
        ASID_BITS = asid.trailing_ones();

        // Set kernel page table with ASID 0 this time (which is reserved for the kernel itself).
        set_page_table(root_ppn, 0);

        // Virtual memory fence to ensure any new things in the kernel page table become visible.
        vmem_fence(None, None);

        // TODO: Detect PBMT once noexc code is supported in Rust.
    }
}

#[inline(always)]
/// Switch page table and address space ID.
pub unsafe fn set_page_table(root_ppn: PPN, asid: u32) {
    #[cfg(target_arch = "riscv32")]
    let new_val = root_ppn + (asid << 22) + (1 << 31);
    #[cfg(target_arch = "riscv64")]
    let new_val =
        root_ppn + ((asid as usize) << 44) + (unsafe { PAGING_LEVELS as usize - 3 + 8 } << 60);
    unsafe { asm!("csrw satp, {new_val}", new_val = in(reg) new_val) };
}

#[inline(always)]
/// Read the current ASID out.
fn read_asid() -> u32 {
    let val: usize;
    unsafe { asm!("csrr {val}, satp", val = out(reg) val) };
    #[cfg(target_arch = "riscv32")]
    let res = (val >> 22) & 0x1ff;
    #[cfg(target_arch = "riscv64")]
    let res = (val >> 44) & 0xffff;
    res as u32
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
