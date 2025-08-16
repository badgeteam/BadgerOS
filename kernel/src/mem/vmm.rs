// SPDX-FileCopyrightText: 2025 Julian Scheffers <julian@scheffers.net>
// SPDX-FileType: SOURCE
// SPDX-License-Identifier: MIT

use core::sync::atomic::AtomicUsize;

use crate::{
    bindings::error::EResult,
    mem::pmm::{PPN, PhysAlloc},
};

/// Unsigned integer that can store a virtual page number.
pub type AtomicVPN = AtomicUsize;
/// Unsigned integer that can store a virtual page number.
pub type VPN = usize;

#[rustfmt::skip]
pub mod flags {
    /// Map memory as executable.
    pub const R:   u32 = 0x0000_0001;
    /// Map memory as writeable (implicitly allows reads).
    pub const W:   u32 = 0x0000_0002;
    /// Map memory as read-write.
    pub const RW:  u32 = 0x0000_0003;
    /// Map memory as executable.
    pub const X:   u32 = 0x0000_0004;
    /// Map memory as read-executable.
    pub const RX:  u32 = 0x0000_0005;
    /// Map memory as read-write-executable.
    pub const RWX: u32 = 0x0000_0007;
    /// Map memory as user-accessible.
    pub const U:   u32 = 0x0000_0010;
    /// Map memory as global (exists in all page ASIDs).
    pub const G:   u32 = 0x0000_0020;
    /// Map memory as I/O (uncached, no write coalescing).
    pub const IO:  u32 = 0x0000_0040;
    /// Map memory as uncached write coalescing.
    pub const NC:  u32 = 0x0000_0080;
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
