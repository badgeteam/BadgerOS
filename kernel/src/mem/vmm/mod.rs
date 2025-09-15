// SPDX-FileCopyrightText: 2025 Julian Scheffers <julian@scheffers.net>
// SPDX-FileType: SOURCE
// SPDX-License-Identifier: MIT

use core::sync::atomic::AtomicUsize;

use crate::{
    bindings::{
        error::{EResult, Errno},
        mutex::Mutex,
    },
    cpu::mmu::BITS_PER_LEVEL,
    mem::{pmm::PPN, vmm::mmu::PAGING_LEVELS},
};

pub mod mmu;

#[rustfmt::skip]
pub mod flags {
    // Note: These flags are the same bit positions as in the RISC-V PTE format, do not change them!
    
    /// Map memory as executable.
    pub const R:   u32 = 0b0000_0000_0010;
    /// Map memory as writeable (reads must also be allowed).
    pub const W:   u32 = 0b0000_0000_0100;
    /// Map memory as executable.
    pub const X:   u32 = 0b0000_0000_1000;
    /// Map memory as user-accessible.
    pub const U:   u32 = 0b0000_0001_0000;
    /// Map memory as global (exists in all page ASIDs).
    pub const G:   u32 = 0b0000_0010_0000;
    /// Page was accessed since this flag was last cleared.
    pub const A:   u32 = 0b0000_0100_0000;
    /// Page was written since this flag was last cleared.
    pub const D:   u32 = 0b0000_1000_0000;
    
    /// Mark page as copy-on-write (W must be disabled).
    pub const COW: u32 = 0b0001_0000_0000;
    
    /// Map memory as I/O (uncached, no write coalescing).
    pub const IO:  u32 = 0b0100_0000_0000;
    /// Map memory as uncached write coalescing.
    pub const NC:  u32 = 0b1000_0000_0000;
    
    /// Map memory as read-write.
    pub const RW:  u32 = R | W;
    /// Map memory as read-execute.
    pub const RX:  u32 = R | X;
    /// Map memory as read-write-execute.
    pub const RWX: u32 = R | W | X;
}

/// Unsigned integer that can store a virtual page number.
pub type AtomicVPN = AtomicUsize;
/// Unsigned integer that can store a virtual page number.
pub type VPN = usize;

/// Kernel page table root PPN.
static KERNEL_PAGE_TABLE: Mutex<PPN> = unsafe { Mutex::new_static(0) };

/// Calculates the maximum page table level wherein the next part of the mapping can be made.
fn calc_superpage(virt_base: VPN, virt_len: VPN, phys_base: PPN) -> u8 {
    let align = virt_base | phys_base;
    ((align.trailing_zeros().min(virt_len.ilog2()) / BITS_PER_LEVEL as u32) as u8)
        .min(unsafe { PAGING_LEVELS })
}

/// Common implementation of [`map_k`], [`map_k_at`] and [`map_u_at`].
unsafe fn map_impl(
    pt_root_ppn: PPN,
    virt_base: VPN,
    virt_len: VPN,
    phys_base: PPN,
    flags: u32,
) -> EResult<()> {
    // Map in original page table.

    if (flags & flags::G) != 0 {
        // Broadcast global mappings to process page tables.
    }

    Ok(())
}

/// Map a range of memory for the kernel at a specific virtual address.
pub unsafe fn map_k_at(virt_base: VPN, virt_len: VPN, phys_base: PPN, flags: u32) -> EResult<()> {
    if flags & flags::U != 0 {
        return Err(Errno::EINVAL);
    }
    let flags = flags | flags::G;
    unsafe {
        map_impl(
            *KERNEL_PAGE_TABLE.lock_shared(),
            virt_base,
            virt_len,
            phys_base,
            flags,
        )
    }
}

/// Finds a certain length of free pages within the higher half.
pub unsafe fn find_free_pages(pt_root_ppn: PPN, virt_len: VPN) -> EResult<VPN> {
    todo!()
}

/// Map a range of memory for the kernel at any virtual address.
/// Returns the virtual page number where it was mapped.
pub unsafe fn map_k(virt_len: VPN, phys_base: PPN, flags: u32) -> EResult<VPN> {
    if flags & flags::U != 0 {
        return Err(Errno::EINVAL);
    }
    let flags = flags | flags::G;
    let guard = KERNEL_PAGE_TABLE.lock();
    let virt_base = unsafe { find_free_pages(*guard, virt_len) }?;
    unsafe { map_impl(*guard, virt_base, virt_len, phys_base, flags) }?;
    Ok(virt_base)
}

/// Map a range of memory for a user page table at a specific virtual address.
pub unsafe fn map_u_at(
    pt_root_ppn: PPN,
    virt_base: VPN,
    virt_len: VPN,
    phys_base: PPN,
    flags: u32,
) -> EResult<()> {
    if flags & !(flags::RWX | flags::A | flags::D) != 0 {
        return Err(Errno::EINVAL);
    }
    let flags = flags | flags::U;
    unsafe { map_impl(pt_root_ppn, virt_base, virt_len, phys_base, flags) }
}
