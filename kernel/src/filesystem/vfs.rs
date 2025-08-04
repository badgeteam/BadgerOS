// SPDX-FileCopyrightText: 2025 Julian Scheffers <julian@scheffers.net>
// SPDX-FileType: SOURCE
// SPDX-License-Identifier: MIT

use core::{
    cell::{OnceCell, UnsafeCell},
    hint::unlikely,
    sync::atomic::{AtomicU32, AtomicU64, Ordering},
};

use alloc::{
    boxed::Box,
    collections::btree_map::BTreeMap,
    sync::{Arc, Weak},
    vec::Vec,
};
use mflags::MFlags;

use super::{Dirent, File, NewFileSpec, NodeType, SeekMode, Stat};
use crate::{
    LogLevel,
    bindings::{
        device::class::{block::BlockDevice, char::CharDevice},
        error::{EResult, Errno},
        filesystem::Media,
        mutex::Mutex,
    },
};

/// A file that is stored in a [`Vfs`].
pub struct VfsFile {
    /// Underlying vnode.
    pub(super) vnode: Arc<VNode>,
    /// Current file position.
    pub(super) offset: AtomicU64,
    /// This handle is in append mode.
    pub(super) is_append: bool,
    /// This handle allows reading.
    pub(super) allow_read: bool,
    /// This handle allows writing.
    pub(super) allow_write: bool,
}

impl VfsFile {
    /// Implementation of append-mode writes.
    fn append_write(&self, wdata: &[u8]) -> EResult<usize> {
        let mut ops = self.vnode.ops.lock();
        let old_size = ops.get_size();
        let new_size = old_size
            .checked_add(wdata.len() as u64)
            .ok_or(Errno::ENOSPC)?;
        ops.resize(new_size)?;
        self.offset.store(new_size, Ordering::Relaxed);
        ops.write(old_size, wdata).map(|_| wdata.len())
    }

    /// Implementation of non-append writes.
    fn regular_write(&self, wdata: &[u8]) -> EResult<usize> {
        let mut ops = self.vnode.ops.lock_shared();
        let mut offset = self.offset.load(Ordering::Relaxed);
        let mut size = ops.get_size();

        loop {
            let new_off = offset
                .checked_add(wdata.len() as u64)
                .ok_or(Errno::ENOSPC)?;

            if new_off > size {
                // The file must be resized.
                drop(ops);
                let mut mut_ops = self.vnode.ops.lock();
                if mut_ops.get_size() == size {
                    mut_ops.resize(new_off)?;
                    size = new_off;
                } else {
                    size = mut_ops.get_size();
                }
                drop(mut_ops);
                ops = self.vnode.ops.lock_shared();
            } else if let Err(x) =
                self.offset
                    .compare_exchange(offset, new_off, Ordering::Relaxed, Ordering::Relaxed)
            {
                // Failed to update offset; try again.
                offset = x;
            } else {
                // Offset updated successfully; perform write.
                return ops.write(offset, wdata).map(|_| wdata.len());
            }
        }
    }
}

impl File for VfsFile {
    fn stat(&self) -> EResult<Stat> {
        self.vnode.ops.lock_shared().stat()
    }

    fn tell(&self) -> EResult<u64> {
        Ok(self.offset.load(Ordering::Relaxed))
    }

    fn seek(&self, mode: SeekMode, offset: i64) -> EResult<u64> {
        let ops = self.vnode.ops.lock_shared();
        let size = ops.get_size();
        let mut old_off = self.offset.load(Ordering::Relaxed);

        let mut new_off = match mode {
            SeekMode::Set => offset.clamp(0, size as i64),
            SeekMode::Cur => offset.saturating_add(old_off as i64).clamp(0, size as i64),
            SeekMode::End => offset.saturating_add(size as i64).clamp(0, size as i64),
        } as u64;

        while let Err(x) =
            self.offset
                .compare_exchange(old_off, new_off, Ordering::Relaxed, Ordering::Relaxed)
        {
            old_off = x;
            new_off = match mode {
                SeekMode::Set => offset.clamp(0, size as i64),
                SeekMode::Cur => offset.saturating_add(old_off as i64).clamp(0, size as i64),
                SeekMode::End => offset.saturating_add(size as i64).clamp(0, size as i64),
            } as u64;
        }

        Ok(new_off)
    }

    fn write(&self, wdata: &[u8]) -> EResult<usize> {
        if !self.allow_write {
            Err(Errno::EBADF)
        } else if self.is_append {
            self.append_write(wdata)
        } else {
            self.regular_write(wdata)
        }
    }

    fn read(&self, rdata: &mut [u8]) -> EResult<usize> {
        if !self.allow_read {
            return Err(Errno::EBADF);
        }

        // Get file ops and size.
        let ops = self.vnode.ops.lock_shared();
        let size = ops.get_size();

        // Increment offset and determine read count.
        let mut offset = self.offset.load(Ordering::Acquire);
        let mut readlen = (rdata.len() as u64).min(size.saturating_sub(offset)) as usize;
        while let Err(x) = self.offset.compare_exchange(
            offset,
            offset + readlen as u64,
            Ordering::Acquire,
            Ordering::Relaxed,
        ) {
            offset = x;
            readlen = (rdata.len() as u64).min(size.saturating_sub(offset)) as usize;
        }

        // Perform read on vnode ops.
        ops.read(offset, &mut rdata[0..readlen])?;
        Ok(readlen)
    }

    fn resize(&self, size: u64) -> EResult<()> {
        let mut ops = self.vnode.ops.lock();
        ops.resize(size)?;
        self.offset
            .update(Ordering::Relaxed, Ordering::Relaxed, |f| f.min(size));
        Ok(())
    }

    fn sync(&self) -> EResult<()> {
        self.vnode.ops.lock_shared().sync()
    }

    fn get_vnode(&self) -> Option<Arc<VNode>> {
        Some(self.vnode.clone())
    }
}

#[rustfmt::skip]
pub mod vnflags {
    /// VNode flags.
    pub type VNFlags = u32;
    /// VNode is removed from the filesystem.
    pub const REMOVED: u32 = 0x0000_0001;
}

/// A virtual generalization of inodes. Multiple [`super::File`]s may refer to one vnode.
pub struct VNode {
    /// VNode operations.
    pub(super) ops: Mutex<Box<dyn VNodeOps>>,
    /// Inode number on the parent filesystem.
    pub(super) inode: u64,
    /// VFS on which this VNode exists.
    pub(super) vfs: Arc<Vfs>,
    /// Dirent cache associated.
    pub(super) dentcache: Option<Arc<DentCache>>,
    /// VNode flags.
    pub(super) flags: AtomicU32,
    /// What kind of node this is.
    pub(super) type_: NodeType,
}

impl Drop for VNode {
    fn drop(&mut self) {
        self.vfs.vnodes.lock().remove(&self.inode);
    }
}

/// Abstract vnode operations.
pub trait VNodeOps {
    /// Get the associated block device, if any.
    fn get_blockdev(&self) -> Option<BlockDevice> {
        None
    }
    /// Get the associated character device, if any.
    fn get_chardev(&self) -> Option<CharDevice> {
        None
    }

    /// Write data to the file.
    fn write(&self, offset: u64, wdata: &[u8]) -> EResult<()>;
    /// Read data from the file.
    fn read(&self, offset: u64, rdata: &mut [u8]) -> EResult<()>;
    /// Resize the file.
    fn resize(&mut self, new_size: u64) -> EResult<()>;

    /// Find a directory entry.
    fn find_dirent(&self, name: &[u8]) -> EResult<Dirent>;
    /// Get all directory entries.
    fn get_dirents(&self) -> EResult<Vec<Dirent>>;
    /// Unlink a node from this directory.
    /// Uses POSIX `rmdir` semantics iff `is_rmdir`, otherwise POSIX unlink semantics.
    fn unlink(&mut self, name: &[u8], is_rmdir: bool) -> EResult<()>;
    /// Link an existing inode to this directory.
    fn link(&mut self, name: &[u8], inode: &VNode) -> EResult<()>;
    /// Create a new file in this directory.
    fn make_file(&mut self, name: &[u8], spec: NewFileSpec) -> EResult<Box<dyn VNodeOps>>;
    /// Rename a file within this directory.
    /// See [`VfsOps::rename`] for renaming between two different directories.
    fn rename(&mut self, old_name: &[u8], new_name: &[u8]) -> EResult<()>;

    /// Read the link if this is a symlink.
    fn readlink(&self) -> EResult<Box<[u8]>>;
    /// Get this node's stat buffer.
    fn stat(&self) -> EResult<Stat>;
    /// Get this node's inode number.
    fn get_inode(&self) -> u64;
    /// Get the current size of the file.
    fn get_size(&self) -> u64;
    /// Get the type of nod this is.
    fn get_type(&self) -> NodeType;
    /// Sync the underlying caches to disk.
    fn sync(&self) -> EResult<()>;
}

#[rustfmt::skip]
pub mod mflags {
    /// Mounted filesystem flags.
    pub type MFlags = u32;
    /// Filesystem is read-only.
    pub const READ_ONLY: u32 = 0x0000_0001;
}

/// A mounted virtual filesystem.
pub struct Vfs {
    /// Instance of the filesystem driver.
    pub(super) ops: Mutex<Box<dyn VfsOps>>,
    /// Dictionary of VNodes that are opened in this VFS.
    pub(super) vnodes: Mutex<BTreeMap<u64, Weak<VNode>>>,
    /// Handle of the root directory.
    pub(super) root: UnsafeCell<Option<Arc<VNode>>>,
    /// Mountpoint of this VFS.
    pub(super) mountpoint: Option<Arc<VNode>>,
    /// Mounted filesystem flags.
    pub(super) flags: AtomicU32,
}
unsafe impl Sync for Vfs {}

impl Vfs {
    /// Helper function to get [`Vfs::root`], which must already be initialized.
    pub(super) fn root(&self) -> Arc<VNode> {
        unsafe {
            self.root
                .as_ref_unchecked()
                .clone()
                .unwrap_unchecked()
                .clone()
        }
    }

    /// Try to get an existing vnode.
    pub(super) fn get_vnode(&self, inode: u64) -> Option<Arc<VNode>> {
        self.vnodes.lock_shared().get(&inode)?.upgrade()
    }

    /// Try to open a vnode.
    /// The caller must guarantee `dirent` is up-to-date.
    fn open(
        self: &Arc<Self>,
        dirent: &Dirent,
        dentcache: Option<Arc<DentCache>>,
    ) -> EResult<Arc<VNode>> {
        if let Some(vnode) = self.get_vnode(dirent.ino) {
            return Ok(vnode);
        }

        let mut guard = self.vnodes.lock();
        if let Some(vnode) = try { guard.get(&dirent.ino)?.upgrade()? } {
            // Race condition: Another thread opened the vnode in the mean time.
            return Ok(vnode);
        }

        // Call the filesystem to open the vnode.
        let ops = self.ops.lock_shared().open(dirent)?;

        let vnode = Arc::try_new(VNode {
            ops: Mutex::new(ops),
            inode: dirent.ino,
            vfs: self.clone(),
            dentcache,
            flags: AtomicU32::new(0),
            type_: dirent.type_,
        })?;

        // Insert the new vnode.
        guard.insert(dirent.ino, Arc::downgrade(&vnode));

        Ok(vnode)
    }

    #[inline(never)]
    /// Called if an I/O error happens on this VFS.
    fn check_eio_failed(&self) {
        logkf!(
            LogLevel::Error,
            "I/O error on filesystem; marking read-only"
        );
        self.flags.fetch_or(mflags::READ_ONLY, Ordering::Relaxed);
    }

    #[inline(always)]
    /// Mark the VFS as readonly and raise a warning if `result` is [`Errno::EIO`].
    pub(super) fn check_eio<T>(&self, result: EResult<T>) -> EResult<T> {
        if unlikely(match &result {
            Err(x) => *x == Errno::EIO,
            _ => false,
        }) {
            self.check_eio_failed();
        }
        result
    }
}

/// Filesystem-wide operations for a [`Vfs`]; instance of a [`VfsDriver`].
pub trait VfsOps: Sync {
    /// Open the root directory.
    fn open_root(&self) -> EResult<Box<dyn VNodeOps>>;
    /// Open a file or directory.
    /// The caller must guarantee `dirent` is up-to-date.
    fn open(&self, dirent: &Dirent) -> EResult<Box<dyn VNodeOps>>;
    /// Rename between two different directories.
    /// See [`VNodeOps::rename`] for renaming within a single directory.
    fn rename(
        &self,
        src_dir: &VNode,
        src_name: &[u8],
        dest_dir: &VNode,
        dest_name: &[u8],
    ) -> EResult<()>;
}

/// A filesystem driver.
pub trait VfsDriver {
    /// Detect the filesystem on some medium.
    fn detect(&self, media: &Media) -> EResult<bool>;
    /// Mount the filesystem on some medium.
    /// Expected to log errors if they are caused by invalid parameters.
    fn mount(&self, media: Option<&Media>, mflags: MFlags) -> EResult<Box<dyn VfsOps>>;
}

/// Data associated with dirent caches for directories.
pub(super) struct DentCacheDir {
    /// Child dentcache nodes.
    pub children: BTreeMap<Box<[u8]>, Weak<DentCache>>,
    /// Filesystem mounted at this location.
    pub mounted: Option<Arc<Vfs>>,
}

impl DentCacheDir {
    pub const EMPTY: DentCacheDir = DentCacheDir {
        children: BTreeMap::new(),
        mounted: None,
    };
}

/// Possible types of dirent cache entry.
pub(super) enum DentCacheType {
    /// Explicitly does not exist.
    Negative,
    /// A directory.
    Directory(Mutex<DentCacheDir>),
    /// A symbolic link.
    Symlink(Box<[u8]>),
    /// Some other kind of file.
    File,
}

impl DentCacheType {
    pub fn as_dir(&self) -> Option<&Mutex<DentCacheDir>> {
        match self {
            Self::Directory(x) => Some(x),
            _ => None,
        }
    }
}

/// A directory cache entry.
pub(super) struct DentCache {
    /// What kind of entry this is.
    pub type_: DentCacheType,
    /// The VFS this resides in.
    pub vfs: Arc<Vfs>,
    /// The parent dirent.
    pub parent: Option<Arc<DentCache>>,
    /// The vnode this is linked to, if any.
    pub vnode: Mutex<Option<Weak<VNode>>>,
    /// This cached dirent.
    pub dirent: Dirent,
}

impl DentCache {
    /// Get the real path this cache entry represents.
    pub fn realpath(self: &Arc<Self>) -> EResult<Vec<u8>> {
        let mut this = self.clone();
        let mut components = Vec::new();

        while let Some(parent) = this.parent.clone() {
            components.try_reserve(1)?;
            components.push(this.clone());
            this = parent;
        }

        let mut path = Vec::new();
        if components.len() == 0 {
            path.try_reserve(1)?;
            path.push(b'/');
        } else {
            for component in components.iter().rev() {
                path.try_reserve(component.dirent.name.len() + 1)?;
                path.push(b'/');
                path.extend(component.dirent.name.iter());
            }
        }

        Ok(path)
    }

    /// Read the symlink target.
    pub fn readlink(&self) -> EResult<&[u8]> {
        if let DentCacheType::Symlink(link) = &self.type_ {
            Ok(link)
        } else {
            Err(Errno::EINVAL)
        }
    }

    /// Look up a name in this directory.
    pub fn lookup(self: &Arc<Self>, component: &[u8]) -> EResult<Arc<DentCache>> {
        Self::lookup_impl(self.clone(), component)
    }

    #[inline(always)]
    /// Look up a name in this directory.
    fn lookup_impl(mut this: Arc<Self>, component: &[u8]) -> EResult<Arc<DentCache>> {
        // Assert this to be a directory.
        let mut cache = if let DentCacheType::Directory(x) = &this.type_ {
            x
        } else {
            return Err(Errno::ENOTDIR);
        };
        let mut guard = cache.lock_shared();

        // Handle mounted filesystems.
        while let Some(mounted) = guard.mounted.clone() {
            drop(guard);
            this = mounted.root().dentcache.clone().unwrap();
            cache = if let DentCacheType::Directory(x) = &this.type_ {
                x
            } else {
                unreachable!();
            };
            guard = cache.lock_shared();
        }

        // Handle `.` and `..` components.
        if component == b"." {
            return Ok(this.clone());
        } else if component == b".." {
            // Get the parent dir.
            if let Some(x) = &this.parent {
                return Ok(x.clone());
            }
            // Traverse back up to the parent VFS' mountpoint.
            drop(guard);
            while let Some(x) = this.vfs.mountpoint.clone() {
                this = x.dentcache.clone().unwrap();
            }
            // Try again to get the parent dir on the new VFS.
            if let Some(x) = &this.parent {
                return Ok(x.clone());
            } else {
                return Ok(this.clone());
            }
        }

        // Try to look up from the cache.
        if let Some(child) = guard.children.get(component) {
            if let Some(arc) = child.upgrade() {
                // It was cached already.
                return Ok(arc);
            }
        }
        drop(guard);

        // Read from the filesystem.
        let mut guard = cache.lock();
        if let Some(child) = guard.children.get(component) {
            if let Some(arc) = child.upgrade() {
                // Race condition: It was cached by another thread while the mutex was not held.
                return Ok(arc);
            }
        }
        let self_vnode = this.open_vnode()?;
        let dirent = match self_vnode.ops.lock_shared().find_dirent(component) {
            Ok(x) => x,
            Err(x) => {
                if x == Errno::ENOENT {
                    // Directory exists but the file requested doesn't.
                    let value = Arc::try_new(DentCache {
                        type_: DentCacheType::Negative,
                        vfs: this.vfs.clone(),
                        parent: Some(this.clone()),
                        vnode: Mutex::new(None),
                        dirent: Dirent {
                            name: component.into(),
                            ..Default::default()
                        },
                    })?;
                    guard
                        .children
                        .insert(component.into(), Arc::downgrade(&value));

                    return Ok(value);
                } else {
                    return Err(x);
                }
            }
        };

        // Insert new entry.
        match dirent.type_ {
            NodeType::Directory => {
                let value = Arc::try_new(DentCache {
                    type_: DentCacheType::Directory(Mutex::new(DentCacheDir::EMPTY)),
                    parent: Some(this.clone()),
                    vnode: Mutex::new(None),
                    vfs: this.vfs.clone(),
                    dirent,
                })?;
                guard
                    .children
                    .insert(component.into(), Arc::downgrade(&value));

                Ok(value)
            }
            NodeType::Symlink => {
                // Read the symlink first.
                let vnode = self_vnode.vfs.open(&dirent, None)?;
                let name = vnode.ops.lock_shared().readlink()?;

                let value = Arc::try_new(DentCache {
                    type_: DentCacheType::Symlink(name),
                    parent: Some(this.clone()),
                    vnode: Mutex::new(Some(Arc::downgrade(&vnode))),
                    vfs: this.vfs.clone(),
                    dirent,
                })?;
                guard
                    .children
                    .insert(component.into(), Arc::downgrade(&value));

                Ok(value)
            }
            _ => {
                // Neither directory nor symlink.
                let value = Arc::try_new(DentCache {
                    type_: DentCacheType::File,
                    parent: Some(this.clone()),
                    vnode: Mutex::new(None),
                    vfs: this.vfs.clone(),
                    dirent,
                })?;
                guard
                    .children
                    .insert(component.into(), Arc::downgrade(&value));

                Ok(value)
            }
        }
    }

    /// Follow any possible mounts on this dentcache.
    pub fn follow_mounts(self: &Arc<Self>) -> Arc<Self> {
        let mut this = self.clone();
        if let DentCacheType::Directory(cache) = &this.type_ {
            let mut cache = cache;
            let mut guard = cache.lock_shared();
            while let Some(mounted) = guard.mounted.clone() {
                drop(guard);
                this = mounted.root().dentcache.clone().unwrap();
                cache = if let DentCacheType::Directory(x) = &this.type_ {
                    x
                } else {
                    unreachable!();
                };
                guard = cache.lock_shared();
            }
        }
        this
    }

    /// Un-follow mounts; returns the parent VFS dentcache if this is a VFS root.
    /// Returns self if this is the root directory.
    pub fn unfollow_mounts(self: &Arc<Self>) -> Arc<Self> {
        let mut this = self.clone();
        while self.parent.is_none()
            && let Some(mountpoint) = self.vfs.mountpoint.clone()
        {
            this = mountpoint.dentcache.clone().unwrap();
        }
        this
    }

    /// Whether this is the root of a VFS.
    pub fn is_vfs_root(&self) -> bool {
        self.parent.is_none()
    }

    /// Get or open the associated VNode.
    pub fn open_vnode(self: &Arc<Self>) -> EResult<Arc<VNode>> {
        if let Some(weak) = &*self.vnode.lock_shared() {
            if let Some(arc) = weak.upgrade() {
                return Ok(arc);
            }
        }

        let mut guard = self.vnode.lock();
        if let Some(weak) = &*guard {
            if let Some(arc) = weak.upgrade() {
                return Ok(arc);
            }
        }

        let vnode = self.vfs.open(
            &self.dirent,
            if self.dirent.type_ == NodeType::Directory {
                Some(self.clone())
            } else {
                None
            },
        )?;

        *guard = Some(Arc::downgrade(&vnode));

        Ok(vnode)
    }
}
