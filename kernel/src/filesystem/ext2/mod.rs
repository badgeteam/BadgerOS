// SPDX-FileCopyrightText: 2025 Julian Scheffers <julian@scheffers.net>
// SPDX-FileType: SOURCE
// SPDX-License-Identifier: MIT

use core::{
    any::Any,
    mem::offset_of,
    num::{NonZeroU32, NonZeroU64},
    sync::atomic::{AtomicU32, Ordering},
};

use alloc::{
    boxed::Box,
    collections::btree_map::BTreeMap,
    sync::{Arc, Weak},
    vec::Vec,
};

use crate::{
    LogLevel,
    badgelib::time::Timespec,
    bindings::{
        device::HasBaseDevice,
        error::{EResult, Errno},
        mutex::Mutex,
        time_us,
    },
    filesystem::NAME_MAX,
    util::{MaybeMut, zeroes},
};
use spec::*;

use super::{
    Dirent, FSDRIVERS, NodeType, Stat,
    media::Media,
    vfs::{VNode, VNodeMtxInner, VNodeOps, Vfs, VfsDriver, VfsOps, mflags::MFlags},
};

mod spec;

struct E2VNode {
    /// Inode number.
    ino: u32,
    /// Cached inode structure.
    inode: Mutex<Inode>,
    /// On-disk inode structure offset.
    inode_offset: u64,
    /// Current size.
    size: u64,
    /// Node type.
    type_: NodeType,
}

impl E2VNode {
    /// Helper function to get one block ID from the inode.
    fn get_block(
        &self,
        arc_self: &Arc<VNode>,
        block: u32,
        de_sparse: bool,
    ) -> EResult<Option<NonZeroU32>> {
        if de_sparse {
            let mut guard = self.inode.lock();
            self.get_block_unlocked(arc_self, block, MaybeMut::Mut(&mut guard))
        } else {
            let guard = self.inode.lock_shared();
            self.get_block_unlocked(arc_self, block, MaybeMut::Const(&guard))
        }
    }

    /// Recursive implementation of `get_block`.
    fn get_block_recursive(
        &self,
        e2fs: &E2Fs,
        level: u8,
        fileblk: u32,
        block_ptr: NonZeroU32,
        group_hint: u32,
        de_sparse: bool,
    ) -> EResult<Option<NonZeroU32>> {
        if level == 0 {
            return Ok(Some(block_ptr));
        }

        let index =
            (fileblk >> (e2fs.block_size_exp * level as u32)) % (1u32 << e2fs.block_size_exp);
        let block_ptr = ((u32::from(block_ptr) as u64) << e2fs.block_size_exp) + 4 * index as u64;
        let mut tmp = [0u8; 4];
        e2fs.media.read(block_ptr, &mut tmp)?;

        let mut block = u32::from_le_bytes(tmp);
        if block == 0 {
            if de_sparse {
                block = e2fs.alloc_block(group_hint)?.into();
                e2fs.media.write(block_ptr, &block.to_le_bytes())?;
            } else {
                return Ok(None);
            }
        }
        let block = unsafe { NonZeroU32::new_unchecked(block) };

        self.get_block_recursive(
            e2fs,
            level - 1,
            fileblk,
            block,
            u32::from(block) / e2fs.blocks_per_group,
            de_sparse,
        )
    }

    /// Helper function to get one block ID from the inode.
    fn get_block_unlocked(
        &self,
        arc_self: &Arc<VNode>,
        block: u32,
        mut inode: MaybeMut<'_, Inode>,
    ) -> EResult<Option<NonZeroU32>> {
        let e2fs = arc_self.vfs.get_ops_as::<E2Fs>();
        let group_hint = (self.ino - 1) / e2fs.inodes_per_group;

        let block_size_exp = e2fs.block_size_exp;
        let block_ptrs_exp = block_size_exp - 2;
        let ind1_blocks = 1u32 << block_ptrs_exp;
        let ind2_blocks = 1u32 << (2 * block_ptrs_exp);
        let ind3_blocks = 1u32 << (3 * block_ptrs_exp);

        let index: usize;
        let level: u8;
        let fileblk: u32;

        if block < 12 {
            // Direct blocks.
            index = block as usize;
            fileblk = 0;
            level = 0;
        } else if block - 12 < ind1_blocks {
            // Indirect blocks.
            index = 12;
            fileblk = block - 12;
            level = 1;
        } else if block - 12 - ind1_blocks < ind2_blocks {
            // Doubly indirect blocks.
            index = 13;
            fileblk = block - 12 - ind1_blocks;
            level = 2;
        } else if block - 12 - ind1_blocks - ind2_blocks < ind3_blocks {
            // Triply indirect blocks.
            index = 13;
            fileblk = block - 12 - ind1_blocks - ind2_blocks;
            level = 3;
        } else {
            unreachable!("Block index into inode too high");
        }

        let block_ptr = NonZeroU32::new(inode.data_blocks[index]);
        let block_ptr = if let Some(x) = block_ptr {
            x
        } else if let Some(inode) = &mut inode.try_mut() {
            let x = e2fs.alloc_block(group_hint)?;
            (*inode).data_blocks[index] = x.into();
            e2fs.media
                .write(self.inode_offset, &Into::<[u8; _]>::into(**inode))?;
            x
        } else {
            return Ok(None);
        };
        self.get_block_recursive(&e2fs, level, fileblk, block_ptr, group_hint, inode.is_mut())
    }

    /// Helper function to iterate blocks within a certain range of this file.
    /// Callback args: file offset, disk offset, length.
    fn iter_blocks(
        &self,
        arc_self: &Arc<VNode>,
        offset: u64,
        length: u64,
        de_sparse: bool,
        cb: &mut dyn FnMut(u64, Option<NonZeroU64>, u64) -> EResult<()>,
    ) -> EResult<()> {
        if length == 0 {
            return Ok(()); // Prevents underflow subtractions
        }
        let e2fs = arc_self.vfs.get_ops_as::<E2Fs>();

        // Convert the range into a range on disk.
        let block_size_exp = e2fs.block_size_exp;
        let fileblk_start = (offset >> block_size_exp) as u32;
        let fileblk_end = (offset + length).div_ceil(1u64 << block_size_exp) as u32;

        // Helper lambda for running the callback function.
        #[inline(always)]
        fn wrapper(
            block_size_exp: u32,
            offset: u64,
            length: u64,
            start: u32,
            end: u32,
            prev: Option<NonZeroU32>,
            cb: &mut dyn FnMut(u64, Option<NonZeroU64>, u64) -> EResult<()>,
        ) -> EResult<()> {
            let startblk = (start as u64) << block_size_exp;
            let endblk = (end as u64) << block_size_exp;
            let diskblk =
                prev.map(|x| ((u32::from(x) - (end - start - 1)) as u64) << block_size_exp);
            let start = startblk.max(offset);
            let end = endblk.min(offset + length);
            let disk = diskblk.map(|x| x + (start - startblk));
            let disk = disk.map(|x| unsafe { NonZeroU64::new_unchecked(x) });
            cb(start, disk, end - start)
        }

        // The start of the range of blocks found.
        let mut start = None::<u32>;
        let mut prev = None::<NonZeroU32>;

        for fileblk in fileblk_start..fileblk_end {
            let mut block = self.get_block(arc_self, fileblk, false)?;
            if de_sparse && block.is_none() {
                block = self.get_block(arc_self, fileblk, true)?;
                debug_assert!(block.is_some());
            }
            if let Some(y) = start
                && prev.map(|x| unsafe { NonZeroU32::new_unchecked(u32::from(x) + 1) }) != block
            {
                wrapper(block_size_exp, offset, length, y, fileblk, prev, cb)?;
                start = Some(fileblk);
            } else if start.is_none() {
                start = Some(fileblk);
            }
            prev = block;
        }
        if let Some(y) = start {
            wrapper(block_size_exp, offset, length, y, fileblk_end, prev, cb)?;
        }

        Ok(())
    }

    /// Helper function to iterate over all dirents.
    /// Assumes the directory to use linked list dirents.
    fn iter_dirents(
        &self,
        arc_self: &Arc<VNode>,
        cb: &mut dyn FnMut(&LinkedDent, u64, &[u8]) -> EResult<bool>,
    ) -> EResult<()> {
        let e2fs = arc_self.vfs.get_ops_as::<E2Fs>();
        let block_size_exp = e2fs.block_size_exp;
        let mut offset = 0u64;
        let mut name = [0u8; NAME_MAX];
        while offset < self.size {
            // Read dirent header.
            let mut dent = [0u8; size_of::<LinkedDent>()];
            self.read(arc_self, offset, &mut dent)?;
            let dent = LinkedDent::from(dent);
            let dent_end = offset.saturating_add(dent.record_len as u64);
            if dent.record_len % 4 != 0 || dent.record_len < size_of::<LinkedDent>() as u16 {
                logkf!(LogLevel::Error, "Dirent has invalid record length");
                arc_self.vfs.check_eio_failed();
                break;
            } else if offset >> block_size_exp != (dent_end - 1) >> block_size_exp {
                logkf!(LogLevel::Error, "Dirent spans block boundary");
                arc_self.vfs.check_eio_failed();
                break;
            } else if dent_end > self.size {
                logkf!(LogLevel::Error, "Dirent overflows end of directory");
                arc_self.vfs.check_eio_failed();
                break;
            }

            // Read dirent name.
            let name_len = if e2fs.feature_incompat & feat::incompat::FILETYPE == 0 {
                // Rev. 0 uses what is later repurposed as file type for name length.
                dent.name_len as u16 + dent.file_type as u16 * 256
            } else {
                dent.name_len as u16
            };
            if name_len as usize > NAME_MAX {
                logkf!(LogLevel::Error, "File name too long");
                arc_self.vfs.check_eio_failed();
            }
            self.read(arc_self, offset + size_of::<LinkedDent>() as u64, &mut name)?;

            // Run dirent callback.
            if !cb(&dent, offset, &name[..name_len as usize])? {
                break;
            }

            // Go to next dirent.
            offset += dent.record_len as u64;
        }
        Ok(())
    }
}

impl VNodeOps for E2VNode {
    fn write(&self, arc_self: &Arc<VNode>, offset: u64, wdata: &[u8]) -> EResult<()> {
        let e2fs = arc_self.vfs.get_ops_as::<E2Fs>();
        self.iter_blocks(
            arc_self,
            offset,
            wdata.len() as u64,
            true,
            &mut |fileoff, diskoff, len| try {
                e2fs.media.write(
                    diskoff.unwrap().into(),
                    &wdata[(fileoff - offset) as usize..(fileoff + len - offset) as usize],
                )?;
            },
        )
    }

    fn read(&self, arc_self: &Arc<VNode>, offset: u64, rdata: &mut [u8]) -> EResult<()> {
        let e2fs = arc_self.vfs.get_ops_as::<E2Fs>();
        self.iter_blocks(
            arc_self,
            offset,
            rdata.len() as u64,
            false,
            &mut |fileoff, diskoff, len| {
                if let Some(diskoff) = diskoff {
                    e2fs.media.read(
                        diskoff.into(),
                        &mut rdata[(fileoff - offset) as usize..(fileoff + len - offset) as usize],
                    )
                } else {
                    rdata[(fileoff - offset) as usize..(fileoff - offset + len) as usize].fill(0);
                    Ok(())
                }
            },
        )
    }

    fn resize(&mut self, arc_self: &Arc<VNode>, new_size: u64) -> EResult<()> {
        let e2fs = arc_self.vfs.get_ops_as::<E2Fs>();
        let old_size = self.size;

        if new_size > old_size {
            // Erase new bytes.
            self.iter_blocks(
                arc_self,
                old_size,
                new_size - old_size,
                false,
                &mut |_fileoff, diskoff, len| try {
                    if let Some(diskoff) = diskoff {
                        e2fs.media.write_zeroes(diskoff.into(), len)?;
                    }
                },
            )?;
        } else {
            // Garbage-collect blocks.
            logkf!(LogLevel::Warning, "TODO: Free blocks when resizing inode");
        }

        // Update size in inode.
        let mut inode = self.inode.lock();
        inode.size = new_size as u32;
        e2fs.media.write(
            self.inode_offset + offset_of!(Inode, size) as u64,
            &inode.size.to_le_bytes(),
        )?;
        if e2fs.feature_ro_compat & feat::compat_ro::LARGE_FILE != 0 {
            inode.dir_acl = (new_size >> 32) as u32;
            e2fs.media.write(
                self.inode_offset + offset_of!(Inode, dir_acl) as u64,
                &inode.dir_acl.to_le_bytes(),
            )?;
        }

        self.size = new_size;
        Ok(())
    }

    fn find_dirent(&self, arc_self: &Arc<VNode>, name: &[u8]) -> EResult<Dirent> {
        let e2fs = arc_self.vfs.get_ops_as::<E2Fs>();
        let mut res = Err(Errno::ENOENT);
        self.iter_dirents(arc_self, &mut |dent, _offset, dent_name| {
            if *name == *dent_name {
                try {
                    let type_ = e2fs.get_inode_type(dent)?;
                    res = Ok(Dirent {
                        ino: dent.ino as u64,
                        type_,
                        name: dent_name.into(),
                        dirent_disk_off: 0,
                        dirent_off: 0,
                    });
                    false
                }
            } else {
                Ok(true)
            }
        })?;
        res
    }

    fn get_dirents(&self, arc_self: &Arc<VNode>) -> EResult<Vec<Dirent>> {
        let e2fs = arc_self.vfs.get_ops_as::<E2Fs>();
        let mut out = Vec::new();
        self.iter_dirents(arc_self, &mut |dent, offset, name| try {
            let type_ = e2fs.get_inode_type(dent)?;
            out.push(Dirent {
                ino: dent.ino as u64,
                type_,
                name: name.into(),
                dirent_disk_off: 0,
                dirent_off: offset,
            });
            true
        })?;
        Ok(out)
    }

    fn unlink(
        &mut self,
        arc_self: &Arc<VNode>,
        name: &[u8],
        is_rmdir: bool,
        unlinked_vnode: Option<Arc<VNode>>,
    ) -> EResult<()> {
        todo!()
    }

    fn link(&mut self, arc_self: &Arc<VNode>, name: &[u8], inode: &VNode) -> EResult<()> {
        todo!()
    }

    fn make_file(
        &mut self,
        arc_self: &Arc<VNode>,
        name: &[u8],
        spec: super::MakeFileSpec,
    ) -> EResult<(Dirent, Box<dyn VNodeOps>)> {
        todo!()
    }

    fn rename(
        &mut self,
        arc_self: &Arc<VNode>,
        old_name: &[u8],
        new_name: &[u8],
    ) -> EResult<Dirent> {
        todo!()
    }

    fn readlink(&self, arc_self: &Arc<VNode>) -> EResult<Box<[u8]>> {
        if self.size > NAME_MAX as u64 {
            return Err(Errno::ENAMETOOLONG);
        }
        let mut link = Vec::try_with_capacity(self.size as usize)?;
        link.resize(self.size as usize, 0);
        if self.size <= 60 {
            let data_blocks = &self.inode.lock_shared().data_blocks;
            let mut block_bytes = [0u8; 60];
            for i in 0..15 {
                block_bytes[i * 4..i * 4 + 4].copy_from_slice(&data_blocks[i].to_le_bytes());
            }
            link.copy_from_slice(&block_bytes[..self.size as usize]);
        } else {
            self.read(arc_self, 0, &mut link)?;
        }
        Ok(link.into())
    }

    fn stat(&self, arc_self: &Arc<VNode>) -> EResult<Stat> {
        let e2fs = arc_self.vfs.get_ops_as::<E2Fs>();
        let inode = self.inode.lock_shared();
        Ok(Stat {
            dev: e2fs
                .media
                .device()
                .as_ref()
                .map(|dev| ((u32::from(dev.id()) as u64) << 32) | dev.class() as u64)
                .unwrap_or(0),
            ino: self.ino as u64,
            mode: inode.mode,
            nlink: inode.nlink,
            uid: inode.uid,
            gid: inode.gid,
            rdev: 0,
            size: self.size,
            blksize: 1u64 << e2fs.block_size_exp,
            blocks: inode.realsize as u64,
            atim: Timespec {
                sec: inode.atime as u64,
                nsec: 0,
            },
            mtim: Timespec {
                sec: inode.mtime as u64,
                nsec: 0,
            },
            ctim: Timespec {
                sec: inode.ctime as u64,
                nsec: 0,
            },
        })
    }

    fn get_inode(&self) -> u64 {
        self.ino as u64
    }

    fn get_size(&self, _arc_self: &Arc<VNode>) -> u64 {
        self.size
    }

    fn get_type(&self, _arc_self: &Arc<VNode>) -> NodeType {
        self.type_
    }

    fn sync(&self, arc_self: &Arc<VNode>) -> EResult<()> {
        let e2fs = arc_self.vfs.get_ops_as::<E2Fs>();
        self.iter_blocks(
            arc_self,
            0,
            self.size,
            false,
            &mut |_fileoff, diskoff, len| {
                if let Some(diskoff) = diskoff {
                    e2fs.media.sync(diskoff.into(), len)
                } else {
                    Ok(())
                }
            },
        )
    }
}

struct E2BlockGroup {
    /// Block group index.
    index: u32,
    /// Cached block group descriptor entry.
    desc: Mutex<BlockGroupDesc>,
}

struct E2Fs {
    /// How many inodes are associated with each block group.
    inodes_per_group: u32,
    /// How many blocks are associated with each block group.
    blocks_per_group: u32,
    /// How many bytes large an inode table entry is.
    inode_size: u16,
    /// Enabled COMPAT features.
    feature_compat: u32,
    /// Enabled INCOMPAT features.
    feature_incompat: u32,
    /// Enabled RO_COMPAT features.
    feature_ro_compat: u32,
    /// Log-base 2 of block size.
    block_size_exp: u32,
    /// Number of block groups.
    block_groups: u32,
    /// Offset of the block group descriptor table.
    bgdt_offset: u64,
    /// Number of available blocks.
    free_blocks: AtomicU32,
    /// Number of available inodes.
    free_inodes: AtomicU32,
    /// Media that the filesystem is mounted on.
    media: Media,
    /// Cached block group descriptors.
    block_group_desc: Mutex<BTreeMap<u32, Weak<E2BlockGroup>>>,
}

impl E2Fs {
    /// Try to allocate a block, starting in block group `group`.
    fn alloc_block(&self, mut group_hint: u32) -> EResult<NonZeroU32> {
        // Reserve one block.
        self.free_blocks
            .fetch_update(Ordering::Relaxed, Ordering::Relaxed, |old| {
                old.checked_sub(1)
            })
            .map_err(|_| Errno::ENOSPC)?;

        loop {
            let group = self.get_block_group(group_hint)?;
            let mut guard = group.desc.lock();

            if guard.free_block_count > 0 {
                for i in 0..self.blocks_per_group.div_ceil(size_of::<usize>() as u32) {
                    // Offset of the part of the bitmap being tested.
                    let offset = ((guard.block_bitmap as u64) << self.block_size_exp)
                        + i as u64 * size_of::<usize>() as u64;

                    // Fetch bytes from the bitmap.
                    let mut tmp = [0u8; size_of::<usize>()];
                    self.media.read(offset, &mut tmp)?;
                    let mut tmp = usize::from_le_bytes(tmp);

                    if tmp != usize::MAX {
                        // Mark block as used.
                        let bitpos = tmp.trailing_ones();
                        tmp |= 1usize << bitpos;
                        self.media.write(offset, &tmp.to_le_bytes())?;

                        // Return the newly allocated block.
                        guard.free_block_count -= 1;
                        return Ok(NonZeroU32::new(
                            bitpos + i as u32 * usize::BITS + guard.block_bitmap,
                        )
                        .unwrap());
                    }
                }

                // There should have been a free block by now.
                return Err(Errno::EIO);
            }

            group_hint = (1 + group_hint) % self.block_groups;
        }
    }

    /// Mark a block as free.
    fn free_block(&self, block: NonZeroU32) -> EResult<()> {
        todo!()
    }

    /// Helper function to get the inode type for a dirent.
    /// Will query the actual inode for ext2 rev. 0.
    fn get_inode_type(&self, dent: &LinkedDent) -> EResult<NodeType> {
        if self.feature_incompat & feat::incompat::FILETYPE == 0 {
            todo!()
        } else {
            Ok(FileType::try_from(dent.file_type)?.into())
        }
    }

    /// Get block group descriptor by block group index.
    fn get_block_group(&self, index: u32) -> EResult<Arc<E2BlockGroup>> {
        if let Some(res) = try { self.block_group_desc.lock_shared().get(&index)?.upgrade()? } {
            return Ok(res.clone());
        }

        let mut guard = self.block_group_desc.lock();
        if let Some(res) = try { guard.get(&index)?.upgrade()? } {
            return Ok(res.clone()); // Race condition: Entry created by another thread.
        }

        let mut group_desc = [0u8; size_of::<BlockGroupDesc>()];
        self.media.read(
            self.bgdt_offset + index as u64 * size_of::<BlockGroupDesc>() as u64,
            &mut group_desc,
        )?;
        let group_desc = BlockGroupDesc::from(group_desc);

        let ent = Arc::try_new(E2BlockGroup {
            index,
            desc: Mutex::new(group_desc),
        })?;
        guard.insert(index, Arc::downgrade(&ent));

        Ok(ent)
    }

    /// Implementation of [`Self::open_root`] and [`Self::open`].
    fn open_impl(&self, ino: u32) -> EResult<Box<dyn VNodeOps>> {
        let block_group = ino.checked_sub(1).ok_or(Errno::EIO)? / self.inodes_per_group;
        let index = (ino - 1) % self.inodes_per_group;

        // Load the inode structure from disk.
        let block_group = self.get_block_group(block_group)?;
        let inode_offset = ((block_group.desc.lock_shared().inode_table as u64)
            << self.block_size_exp)
            + index as u64 * self.inode_size as u64;
        let mut inode = [0u8; size_of::<Inode>()];
        self.media.read(inode_offset, &mut inode)?;
        let inode = Inode::from(inode);

        // Inode size field depends on Ext2 revision and whether it's a directory.
        let size = if self.feature_incompat & feat::incompat::FILETYPE == 0
            || inode.mode & 0xf000 == Mode::Directory as u16
        {
            inode.size as u64
        } else {
            inode.size as u64 | ((inode.dir_acl as u64) << 32)
        };
        let mode = Mode::try_from(inode.mode)?;
        let type_: NodeType = mode.into();

        // Loaded successfully, make VNode.
        Ok(Box::<dyn VNodeOps>::from(Box::try_new(E2VNode {
            ino,
            inode: Mutex::new(inode),
            inode_offset,
            size,
            type_,
        })?))
    }
}

impl VfsOps for E2Fs {
    fn media(&self) -> Option<&Media> {
        Some(&self.media)
    }

    fn uses_inodes(&self) -> bool {
        true
    }

    fn open_root(&self, _arc_self: &Arc<Vfs>) -> EResult<Box<dyn VNodeOps>> {
        self.open_impl(ROOT_INO)
    }

    fn open(&self, _arc_self: &Arc<Vfs>, dirent: &Dirent) -> EResult<Box<dyn VNodeOps>> {
        self.open_impl(dirent.ino as u32)
    }

    fn rename(
        &self,
        arc_self: &Arc<Vfs>,
        src_dir: &Arc<VNode>,
        src_name: &[u8],
        src_mutexinner: &mut VNodeMtxInner,
        dest_dir: &Arc<VNode>,
        dest_name: &[u8],
        dest_mutexinner: &mut VNodeMtxInner,
    ) -> EResult<Dirent> {
        todo!()
    }
}

/// What EXT2 compat features are supported.
pub const COMPAT_SUPPORTED: u32 = 0;
/// What EXT2 incompat features are supported.
pub const INCOMPAT_SUPPORTED: u32 = feat::incompat::FILETYPE;
/// What EXT2 ro-compat features are supported.
pub const RO_COMPAT_SUPPORTED: u32 = feat::compat_ro::LARGE_FILE | feat::compat_ro::SPARSE_SUPER;

struct E2FsDriver {}

impl VfsDriver for E2FsDriver {
    fn detect(&self, media: &Media) -> EResult<bool> {
        // Load the superblock.
        let mut superblock = [0u8; size_of::<Superblock>()];
        media.read(1024, &mut superblock)?;
        let superblock = Superblock::from(superblock);

        // Check the magic value.
        Ok(superblock.magic == MAGIC)
    }

    fn mount(&self, media: Option<Media>, _mflags: MFlags) -> EResult<Box<dyn VfsOps>> {
        let media = media.ok_or(Errno::ENODEV)?;

        // Load the superblock.
        let mut superblock = [0u8; size_of::<Superblock>()];
        media.read(1024, &mut superblock)?;
        let superblock = Superblock::from(superblock);
        let block_size_exp = superblock.block_size_exp + 10;

        // Check the magic value.
        if superblock.magic != MAGIC {
            logkf!(LogLevel::Error, "Bad or missing superblock");
            return Err(Errno::EIO);
        }

        // Set feature levels if rev >= 1.
        let mut feature_compat = 0u32;
        let mut feature_incompat = 0u32;
        let mut feature_ro_compat = 0u32;
        if superblock.rev_level >= 1 {
            feature_compat = superblock.feature_compat;
            feature_incompat = superblock.feature_incompat;
            feature_ro_compat = superblock.feature_ro_compat;

            // Check compatibility flags.
            if superblock.feature_incompat & !INCOMPAT_SUPPORTED != 0 {
                logkf!(
                    LogLevel::Error,
                    "Some INCOMPAT (0x{:08x}) features not supported (0x{:08x})",
                    superblock.feature_incompat as u32,
                    INCOMPAT_SUPPORTED
                );
                return Err(Errno::ENOTSUP);
            }
            if superblock.feature_ro_compat & !RO_COMPAT_SUPPORTED != 0 {
                logkf!(
                    LogLevel::Warning,
                    "Some RO_COMPAT (0x{:08x}) features not supported (0x{:08x})",
                    superblock.feature_ro_compat as u32,
                    RO_COMPAT_SUPPORTED
                );
            }
            if superblock.feature_compat & !COMPAT_SUPPORTED != 0 {
                logkf!(
                    LogLevel::Info,
                    "Some COMPAT (0x{:08x}) features not supported (0x{:08x})",
                    superblock.feature_compat as u32,
                    COMPAT_SUPPORTED
                );
            }
        }

        if superblock.frag_size_exp != superblock.block_size_exp {
            logkf!(
                LogLevel::Error,
                "Different block size and fragment size is not supported"
            );
            return Err(Errno::ENOTSUP);
        }

        // Construct VFS.
        let block_groups = superblock.block_count.div_ceil(superblock.blocks_per_group);
        let vfs = Box::try_new(E2Fs {
            block_size_exp,
            bgdt_offset: (superblock.first_data_block as u64 + 1) << block_size_exp,
            feature_compat,
            feature_incompat,
            feature_ro_compat,
            block_groups,
            media,
            block_group_desc: Mutex::new(BTreeMap::new()),
            inodes_per_group: superblock.inodes_per_group,
            inode_size: superblock.inode_size,
            blocks_per_group: superblock.blocks_per_group,
            free_blocks: AtomicU32::new(0),
            free_inodes: AtomicU32::new(0),
        })?;

        // Test the number of free blocks and inodes.
        for group in 0..block_groups {
            let group = vfs.get_block_group(group)?;
            let guard = group.desc.lock();
            vfs.free_blocks
                .fetch_add(guard.free_block_count as u32, Ordering::Relaxed);
            vfs.free_inodes
                .fetch_add(guard.free_inode_count as u32, Ordering::Relaxed);
        }

        Ok(Box::<dyn VfsOps>::from(vfs))
    }
}

fn register_e2fs() {
    FSDRIVERS
        .lock()
        .insert("ext2".into(), Box::new(E2FsDriver {}));
}

register_kmodule!(ext2, [1, 0, 0], register_e2fs);
