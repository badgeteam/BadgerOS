// SPDX-FileCopyrightText: 2025 Julian Scheffers <julian@scheffers.net>
// SPDX-FileType: SOURCE
// SPDX-License-Identifier: MIT

use alloc::{boxed::Box, sync::Arc, vec::Vec};

use crate::bindings::error::EResult;

use super::{
    Dirent, FSDRIVERS, NodeType, Stat,
    media::Media,
    vfs::{VNode, VNodeMtxInner, VNodeOps, Vfs, VfsDriver, VfsOps, mflags::MFlags},
};

mod spec;

pub struct E2VNode {}

impl VNodeOps for E2VNode {
    fn write(&self, arc_self: &Arc<VNode>, offset: u64, wdata: &[u8]) -> EResult<()> {
        todo!()
    }

    fn read(&self, arc_self: &Arc<VNode>, offset: u64, rdata: &mut [u8]) -> EResult<()> {
        todo!()
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
        todo!()
    }

    fn stat(&self, arc_self: &Arc<VNode>) -> EResult<Stat> {
        todo!()
    }

    fn get_inode(&self) -> u64 {
        todo!()
    }

    fn get_size(&self, arc_self: &Arc<VNode>) -> u64 {
        todo!()
    }

    fn get_type(&self, arc_self: &Arc<VNode>) -> NodeType {
        todo!()
    }

    fn sync(&self, arc_self: &Arc<VNode>) -> EResult<()> {
        todo!()
    }
}

pub struct E2Fs {
    /// Cached superblock.
    superblock: Box<spec::Superblock>,
}

impl VfsOps for E2Fs {
    fn media(&self) -> Option<&Media> {
        todo!()
    }

    fn uses_inodes(&self) -> bool {
        todo!()
    }

    fn open_root(&self, self_arc: &Arc<Vfs>) -> EResult<Box<dyn VNodeOps>> {
        todo!()
    }

    fn open(&self, self_arc: &Arc<Vfs>, dirent: &Dirent) -> EResult<Box<dyn VNodeOps>> {
        todo!()
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

pub struct E2FsDriver {}

impl VfsDriver for E2FsDriver {
    fn detect(&self, media: &Media) -> EResult<bool> {
        todo!()
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
