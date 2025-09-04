// SPDX-FileCopyrightText: 2025 Julian Scheffers <julian@scheffers.net>
// SPDX-FileType: SOURCE
// SPDX-License-Identifier: MIT

use static_assertions::assert_eq_size;

/// Calls conversion function `$func` on integers in `$type`.
#[rustfmt::skip]
macro_rules! int_conv {
    // Implementations on all primitive integer types.
    ($func: ident,  $name: expr,  u8  ) => { $name = u8  ::$func($name); };
    ($func: ident,  $name: expr,  i8  ) => { $name = i8  ::$func($name); };
    ($func: ident,  $name: expr,  u16 ) => { $name = u16 ::$func($name); };
    ($func: ident,  $name: expr,  i16 ) => { $name = i16 ::$func($name); };
    ($func: ident,  $name: expr,  u32 ) => { $name = u32 ::$func($name); };
    ($func: ident,  $name: expr,  i32 ) => { $name = i32 ::$func($name); };
    ($func: ident,  $name: expr,  u64 ) => { $name = u64 ::$func($name); };
    ($func: ident,  $name: expr,  i64 ) => { $name = i64 ::$func($name); };
    ($func: ident,  $name: expr,  u128) => { $name = u128::$func($name); };
    ($func: ident,  $name: expr,  i128) => { $name = i128::$func($name); };
    // Implementation on arrays.
    ($func: ident,  $name: expr,  [$type: tt; $count: expr]) => {
        for __i in 0..$count {
            int_conv!{ $func, $name[__i], $type }
        }
    };
}

/// Defines an on-disk struct with some helper functions.
macro_rules! struct_def {
    (
        // Parse struct definition.
        $(#[doc = $structdoc: expr])?
        struct $struct: ident {
            $(
                $(#[doc = $namedoc: expr])?
                $name: ident : $type: tt
            ),*
            $(,)?
        }
    ) => {
        // Define base struct.
        #[derive(Clone, Copy)]
        #[repr(packed)]
        $(#[doc = $structdoc])?
        pub struct $struct {
            $(
                $(#[doc = $namedoc])?
                pub $name: $type,
            )*
        }
        // Define conversion from bytes.
        impl ::core::convert::From<[u8; ::core::mem::size_of::<$struct>()]> for $struct {
            fn from(value: [u8; ::core::mem::size_of::<$struct>()]) -> Self {
                let mut tmp: Self = unsafe { ::core::mem::transmute(value) };
                $(int_conv!{from_le, tmp.$name, $type})*
                tmp
            }
        }
        // Define conversion to bytes.
        impl ::core::convert::Into<[u8; ::core::mem::size_of::<$struct>()]> for $struct {
            fn into(mut self) -> [u8; ::core::mem::size_of::<$struct>()] {
                $(int_conv!{to_le, self.$name, $type})*
                unsafe { ::core::mem::transmute(self) }
            }
        }
        // Define default to be zeroed.
        impl ::core::default::Default for $struct {
            fn default() -> Self {
                unsafe { ::core::mem::zeroed() }
            }
        }
    };
}

struct_def! {
    /// EXT2 superblock; filesystem metadata.
    struct Superblock {
        /* ==== Generic superblock information ==== */
        inode_count:       u32,
        block_count:       u32,
        resvd_block_count: u32,
        free_block_count:  u32,
        free_inode_count:  u32,
        first_data_block:  u32,
        block_size_exp:    u32,
        frag_size_exp:     u32,
        blocks_per_group:  u32,
        frags_per_group:   u32,
        inodes_per_group:  u32,
        mtime:             u32,
        wtime:             u32,
        mnt_count:         u16,
        max_mnt_count:     u16,
        magic:             u16,
        state:             u16,
        errors:            u16,
        minor_rev_level:   u16,
        lastcheck:         u32,
        checkinterval:     u32,
        creator_os:        u32,
        rev_level:         u32,
        def_resuid:        u16,
        def_resgid:        u16,

        /* ==== EXT2_DYNAMIC_REV Specific ==== */
        first_ino:         u32,
        inode_size:        u16,
        block_group_nr:    u16,
        feature_compat:    u32,
        feature_incompat:  u32,
        feature_ro_compat: u32,
        uuid:              u128,
        volume_name:       [u8; 16],
        last_mounted:      [u8; 64],
        algo_bitmap:       u32,

        /* ==== Performance hints ==== */
        prealloc_blocks:     u8,
        prealloc_dir_blocks: u8,
        _padding0:           [u8; 2],

        /* ==== Journalling support ==== */
        journal_uuid: u128,
        journal_inum: u32,
        journal_dev:  u32,
        last_orphan:  u32,

        /* ==== Directory indexing support ==== */
        hash_seed:    [u32; 4],
        hash_version: u8,
        _padding1:    [u8; 3],

        /* ==== Other options ==== */
        default_mount_options: u32,
        first_meta_blockgroup: u32,
        _padding3:             [u8; 760],
    }
}
assert_eq_size!(Superblock, [u8; 1024]);

struct_def! {
    /// Block group descriptor table.
    struct BlockGroup {
        block_bitmap:     u32,
        inode_bitmap:     u32,
        inode_table:      u32,
        free_block_count: u16,
        free_inode_count: u16,
        used_dirs_count:  u16,
        pad:              u16,
        _padding0:        [u8; 12],
    }
}
assert_eq_size!(BlockGroup, [u8; 32]);

struct_def! {
    /// Inode structure.
    struct Inode {
        mode:       u16,
        uid:        u16,
        size:       u32,
        atime:      u32,
        ctime:      u32,
        mtime:      u32,
        dtime:      u32,
        gid:        u16,
        links:      u16,
        blocks:     u32,
        flags:      u32,
        _osd1:      [u8; 4],
        block:      [u32; 15],
        generation: u32,
        file_acl:   u32,
        /// For regular files, high 32 bits of size.
        dir_acl:    u32,
        _obsolete:  u32,
        _osd2:      [u8; 12],
    }
}
assert_eq_size!(Inode, [u8; 128]);
