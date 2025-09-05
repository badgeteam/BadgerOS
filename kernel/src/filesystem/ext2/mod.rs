// SPDX-FileCopyrightText: 2025 Julian Scheffers <julian@scheffers.net>
// SPDX-FileType: SOURCE
// SPDX-License-Identifier: MIT

use alloc::{
    boxed::Box,
    collections::btree_map::BTreeMap,
    sync::{Arc, Weak},
    vec::Vec,
};

use crate::{
    bindings::{
        error::{EResult, Errno},
        mutex::Mutex,
        raw::timestamp_us_t,
    },
    filesystem::NAME_MAX,
};
use spec::*;

use super::{
    Dirent, FSDRIVERS, NodeType, Stat,
    media::Media,
    vfs::{VNode, VNodeMtxInner, VNodeOps, Vfs, VfsDriver, VfsOps, mflags::MFlags},
};

mod spec;

/// Helper function to get the RAMFS from a VFS.
#[inline(always)]
fn get_e2fs(vfs: &Arc<Vfs>) -> &E2Fs {
    unsafe { &*(vfs.ops.data().as_ref() as *const dyn VfsOps as *const E2Fs) }
}

struct E2VNode {
    /// Inode number.
    ino: u32,
    /// Cached inode structure.
    inode: Inode,
    /// On-disk inode structure offset.
    inode_offset: u64,
    /// Current size.
    size: u64,
    /// Node type.
    type_: NodeType,
}

impl E2VNode {
    /// Helper function to iterate blocks within a certain range of this file.
    /// Callback args: file offset, disk offset, length.
    fn iter_blocks(
        &self,
        arc_self: &Arc<VNode>,
        offset: u64,
        length: u64,
        func: &mut dyn FnMut(u64, u64, u64) -> EResult<()>,
    ) -> EResult<()> {
        todo!()
    }
}

impl VNodeOps for E2VNode {
    fn write(&self, arc_self: &Arc<VNode>, offset: u64, wdata: &[u8]) -> EResult<()> {
        let e2fs = get_e2fs(&arc_self.vfs);
        self.iter_blocks(
            arc_self,
            offset,
            wdata.len() as u64,
            &mut |fileoff, diskoff, len| {
                e2fs.media
                    .write(diskoff, &wdata[fileoff as usize..(fileoff + len) as usize])
            },
        )
    }

    fn read(&self, arc_self: &Arc<VNode>, offset: u64, rdata: &mut [u8]) -> EResult<()> {
        let e2fs = get_e2fs(&arc_self.vfs);
        self.iter_blocks(
            arc_self,
            offset,
            rdata.len() as u64,
            &mut |fileoff, diskoff, len| {
                e2fs.media.read(
                    diskoff,
                    &mut rdata[fileoff as usize..(fileoff + len) as usize],
                )
            },
        )
    }

    fn resize(&mut self, arc_self: &Arc<VNode>, new_size: u64) -> EResult<()> {
        todo!()
    }

    fn find_dirent(&self, arc_self: &Arc<VNode>, name: &[u8]) -> EResult<Dirent> {
        todo!()
    }

    fn get_dirents(&self, arc_self: &Arc<VNode>) -> EResult<Vec<Dirent>> {
        todo!()
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
            let mut block_bytes = [0u8; 60];
            for i in 0..15 {
                block_bytes[i * 4..i * 4 + 4].copy_from_slice(&self.inode.block[i].to_le_bytes());
            }
            link.copy_from_slice(&block_bytes[..self.size as usize]);
        } else {
            self.read(arc_self, 0, &mut link)?;
        }
        Ok(link.into())
    }

    fn stat(&self, arc_self: &Arc<VNode>) -> EResult<Stat> {
        todo!()
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
        let e2fs = get_e2fs(&arc_self.vfs);
        self.iter_blocks(arc_self, 0, self.size, &mut |_fileoff, diskoff, len| {
            e2fs.media.sync(diskoff, len)
        })
    }
}

struct E2BlockGroup {
    /// Block group index.
    index: u32,
    /// Cached block group descriptor entry.
    desc: Mutex<BlockGroupDesc>,
}

struct E2Fs {
    /// Cached superblock.
    superblock: Box<Superblock>,
    /// Blocks used per copy of the block group descriptor table.
    bgdt_blocks: u32,
    /// Offset of the block group descriptor table.
    bgdt_offset: u64,
    /// Media that the filesystem is mounted on.
    media: Media,
    /// Cached block group descriptors.
    block_group_desc: Mutex<BTreeMap<u32, Weak<E2BlockGroup>>>,
}

impl E2Fs {
    /// Get block group descriptor by block group index.
    fn get_block_group(&self, self_arc: &Arc<Vfs>, index: u32) -> EResult<Arc<E2BlockGroup>> {
        todo!()
    }

    /// Implementation of [`Self::open_root`] and [`Self::open`].
    fn open_impl(&self, self_arc: &Arc<Vfs>, ino: u32) -> EResult<Box<dyn VNodeOps>> {
        let block_group = ino.checked_sub(1).ok_or(Errno::EIO)? / self.superblock.inodes_per_group;
        let index = (ino - 1) % self.superblock.inodes_per_group;

        // Load the inode structure from disk.
        let block_group = self.get_block_group(self_arc, block_group)?;
        let inode_offset = ((block_group.desc.lock_shared().inode_table as u64)
            << self.superblock.block_size_exp)
            + index as u64 * size_of::<Inode>() as u64;
        let mut inode = [0u8; size_of::<Inode>()];
        self.media.read(inode_offset, &mut inode)?;
        let inode = Inode::from(inode);

        // Inode size field depends on Ext2 revision and whether it's a directory.
        let size =
            if self.superblock.rev_level == 0 || inode.mode & 0xf000 == Mode::Directory as u16 {
                inode.size as u64
            } else {
                inode.size as u64 | ((inode.dir_acl as u64) << 32)
            };
        let mode = Mode::try_from(inode.mode)?;
        let type_: NodeType = mode.into();

        // Loaded successfully, make VNode.
        Ok(Box::<dyn VNodeOps>::from(Box::try_new(E2VNode {
            ino,
            inode,
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

    fn open_root(&self, self_arc: &Arc<Vfs>) -> EResult<Box<dyn VNodeOps>> {
        self.open_impl(self_arc, ROOT_INO)
    }

    fn open(&self, self_arc: &Arc<Vfs>, dirent: &Dirent) -> EResult<Box<dyn VNodeOps>> {
        self.open_impl(self_arc, dirent.ino as u32)
    }

    fn rename(
        &self,
        self_arc: &Arc<Vfs>,
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

struct E2FsDriver {}

impl VfsDriver for E2FsDriver {
    fn detect(&self, media: &Media) -> EResult<bool> {
        // Load the superblock.
        let mut superblock = Box::new([0u8; 1024]);
        media.read(1024, superblock.as_mut())?;
        let superblock = Box::<Superblock>::from(superblock);

        // Check the magic value.
        Ok(superblock.magic == MAGIC)
    }

    fn mount(&self, media: Option<Media>, mflags: MFlags) -> EResult<Box<dyn VfsOps>> {
        todo!()
    }
}

fn register_e2fs() {
    FSDRIVERS
        .lock()
        .insert("ext2".into(), Box::new(E2FsDriver {}));
}

// register_kmodule!(e2fs, [1, 0, 0], register_e2fs);
