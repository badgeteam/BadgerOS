// SPDX-FileCopyrightText: 2025 Julian Scheffers <julian@scheffers.net>
// SPDX-FileType: SOURCE
// SPDX-License-Identifier: MIT

use zerocopy_derive::{FromBytes, Immutable, KnownLayout};

/// Flattened Device Tree header.
#[derive(Clone, Copy, KnownLayout, FromBytes, Immutable)]
pub struct FdtHeader {
    pub magic: u32,
    pub totalsize: u32,
    pub struct_offset: u32,
    pub string_offset: u32,
    pub memresv_offset: u32,
    pub version: u32,
    pub compat_version: u32,
    pub bsp_cpuid: u32,
    pub string_size: u32,
    pub struct_size: u32,
}

impl FdtHeader {
    /// FDT header magic value.
    pub const MAGIC: u32 = 0xd00dfeed;

    /// Convert all integers into big-endian; byte-swap on little-endian machines.
    pub fn from_be(mut self) -> Self {
        self.magic = u32::from_be(self.magic);
        self.totalsize = u32::from_be(self.totalsize);
        self.struct_offset = u32::from_be(self.struct_offset);
        self.string_offset = u32::from_be(self.string_offset);
        self.memresv_offset = u32::from_be(self.memresv_offset);
        self.version = u32::from_be(self.version);
        self.compat_version = u32::from_be(self.compat_version);
        self.bsp_cpuid = u32::from_be(self.bsp_cpuid);
        self.string_size = u32::from_be(self.string_size);
        self.struct_size = u32::from_be(self.struct_size);
        self
    }

    /// Convert all integers from big-endian; byte-swap on little-endian machines.
    pub fn to_be(self) -> Self {
        self.from_be()
    }
}

pub const FDT_BEGIN_NODE: u32 = 1;
pub const FDT_END_NODE: u32 = 2;
pub const FDT_PROP: u32 = 3;
pub const FDT_NOP: u32 = 4;
pub const FDT_END: u32 = 5;
