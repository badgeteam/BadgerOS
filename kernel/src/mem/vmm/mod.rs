// SPDX-FileCopyrightText: 2025 Julian Scheffers <julian@scheffers.net>
// SPDX-FileType: SOURCE
// SPDX-License-Identifier: MIT

use core::sync::atomic::AtomicUsize;

use crate::{
    bindings::error::EResult,
    mem::pmm::{PPN, PhysAlloc},
};

pub mod mmu;

/// Unsigned integer that can store a virtual page number.
pub type AtomicVPN = AtomicUsize;
/// Unsigned integer that can store a virtual page number.
pub type VPN = usize;

#[rustfmt::skip]
pub mod flags {
    // Note: These flags are the same bit positions as in the RISC-V PTE format, do not change them!
    
    /// Map memory as executable.
    pub const R:   u32 = 0b00_0000_0010;
    /// Map memory as writeable (implicitly allows reads).
    pub const W:   u32 = 0b00_0000_0100;
    /// Map memory as executable.
    pub const X:   u32 = 0b00_0000_1000;
    /// Map memory as user-accessible.
    pub const U:   u32 = 0b00_0001_0000;
    /// Map memory as global (exists in all page ASIDs).
    pub const G:   u32 = 0b00_0010_0000;
    /// Page was accessed since this flag was last cleared.
    pub const A:   u32 = 0b00_0100_0000;
    /// Page was written since this flag was last cleared.
    pub const D:   u32 = 0b00_1000_0000;
    /// Map memory as I/O (uncached, no write coalescing).
    pub const IO:  u32 = 0b01_0000_0000;
    /// Map memory as uncached write coalescing.
    pub const NC:  u32 = 0b10_0000_0000;
    
}

/// Map a range of memory for the kernel at a specific virtual address.
pub unsafe fn map_k_at(
    virt_base: VPN,
    virt_len: VPN,
    phys_base: PPN,
    flags: u32,
    pt_alloc: &dyn PhysAlloc,
) -> EResult<()> {
    todo!()
}

/// Map a range of memory for the kernel at any virtual address.
/// Returns the virtual page number where it was mapped.
pub unsafe fn map_k(
    virt_len: VPN,
    phys_base: PPN,
    flags: u32,
    pt_alloc: &dyn PhysAlloc,
) -> EResult<VPN> {
    todo!()
}
