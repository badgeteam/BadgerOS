// SPDX-FileCopyrightText: 2025 Julian Scheffers <julian@scheffers.net>
// SPDX-FileType: SOURCE
// SPDX-License-Identifier: MIT

use core::{
    any::Any,
    cell::UnsafeCell,
    sync::atomic::{AtomicU64, AtomicUsize, Ordering},
};

use alloc::{boxed::Box, collections::btree_map::BTreeMap, sync::Arc, vec::Vec};

use crate::{
    LogLevel,
    bindings::{
        device::class::{block::BlockDevice, char::CharDevice},
        error::{EResult, Errno},
        irq,
        mutex::Mutex,
        spinlock::Spinlock,
    },
    filesystem::vfs::mflags,
    time::{AtomicTimespec, Timespec},
};

use super::{
    Dirent, FSDRIVERS, MakeFileSpec, NodeMode, NodeType, Stat,
    media::Media,
    vfs::{VNode, VNodeOps, VfsDriver, VfsOps, mflags::MFlags},
};

/// A filesystem that is entirely resident in RAM.
pub struct RamFS {
    /// Inodes table.
    inodes: Mutex<BTreeMap<u64, Arc<UnsafeCell<RamINode>>>>,
    /// Inode number counter.
    ino_ctr: AtomicU64,
    /// Allows for device files.
    allow_devfiles: bool,
}
unsafe impl Sync for RamFS {}

impl RamFS {
    pub fn new(allow_devfiles: bool) -> EResult<Arc<Self>> {
        let fs = Box::try_new(RamFS {
            allow_devfiles,
            inodes: Mutex::new(BTreeMap::new()),
            ino_ctr: AtomicU64::new(2),
        })?;
        let now = Timespec::now();
        let root = Arc::try_new(UnsafeCell::new(RamINode {
            data: RamFSData::Directory(BTreeMap::new()),
            size: AtomicUsize::new(0),
            links: Spinlock::new(0),
            ino: 1,
            atim: AtomicTimespec::new(now),
            mtim: AtomicTimespec::new(now),
            ctim: AtomicTimespec::new(now),
        }))?;
        fs.inodes.lock().insert(1, root);
        Ok(fs.into())
    }

    fn open_impl(self: &Arc<Self>, ino: u64) -> EResult<Box<dyn VNodeOps>> {
        let inode = self
            .inodes
            .lock_shared()
            .get(&ino)
            .ok_or(Errno::EIO)?
            .clone();
        Ok(Box::<dyn VNodeOps>::from(Box::try_new(RamVNode {
            vfs: self.clone(),
            inode,
        })?))
    }
}

impl VfsOps for Arc<RamFS> {
    fn open_root(&self) -> EResult<Box<dyn VNodeOps>> {
        self.open_impl(1)
    }

    fn open(&self, dirent: &Dirent) -> EResult<Box<dyn VNodeOps>> {
        self.open_impl(dirent.ino)
    }

    fn rename(
        &self,
        src_dir: &VNode,
        src_name: &[u8],
        dest_dir: &VNode,
        dest_name: &[u8],
    ) -> EResult<()> {
        // Downcast trait objects into RamVNode.
        let mut src_ops = src_dir.ops.lock();
        let mut dest_ops = dest_dir.ops.lock();
        let src_ramnode = (&mut *src_ops as &mut dyn Any)
            .downcast_mut::<RamVNode>()
            .unwrap();
        let dest_ramnode = (&mut *dest_ops as &mut dyn Any)
            .downcast_mut::<RamVNode>()
            .unwrap();

        // Get RamINode refs.
        let src_inode = unsafe { src_ramnode.inode.as_mut_unchecked() };
        let dest_inode = unsafe { dest_ramnode.inode.as_mut_unchecked() };

        // Get the source dirent.
        let mut entry = src_inode
            .data
            .as_directory_mut()
            .unwrap()
            .remove(src_name)
            .ok_or(Errno::ENOENT)?;

        // Check if destination already exists.
        let dest_dir_map = dest_inode.data.as_directory_mut().unwrap();
        if dest_dir_map.contains_key(dest_name) {
            return Err(Errno::EEXIST);
        }

        // Insert the entry under the new name.
        entry.name = dest_name.into(); // TODO: OOM handling.
        dest_dir_map.insert(dest_name.into(), entry);

        Ok(())
    }
}

/// Data stored in a [`RamINode`].
enum RamFSData {
    /// Named pipe.
    Fifo,
    /// Character device.
    CharDev(CharDevice),
    /// Directory.
    Directory(BTreeMap<Box<[u8]>, Dirent>),
    /// Block device.
    BlockDev(BlockDevice),
    /// Regular file.
    Regular(Vec<u8>),
    /// Symbolic link.
    Symlink(Box<[u8]>),
    /// UNIX domain socket.
    UnixSocket,
}

impl RamFSData {
    /// Get as directory.
    fn as_directory(&self) -> Option<&BTreeMap<Box<[u8]>, Dirent>> {
        match self {
            Self::Directory(x) => Some(x),
            _ => None,
        }
    }
    /// Get as directory (mutable).
    fn as_directory_mut(&mut self) -> Option<&mut BTreeMap<Box<[u8]>, Dirent>> {
        match self {
            Self::Directory(x) => Some(x),
            _ => None,
        }
    }
    /// Get as regular file.
    fn as_regular(&self) -> Option<&Vec<u8>> {
        match self {
            Self::Regular(x) => Some(x),
            _ => None,
        }
    }
    /// Get as regular file (mutable).
    fn as_regular_mut(&mut self) -> Option<&mut Vec<u8>> {
        match self {
            Self::Regular(x) => Some(x),
            _ => None,
        }
    }
    /// Get as symlink.
    fn as_symlink(&self) -> Option<&Box<[u8]>> {
        match self {
            Self::Symlink(x) => Some(x),
            _ => None,
        }
    }

    /// Get the matching [`NodeType`].
    fn node_type(&self) -> NodeType {
        match self {
            RamFSData::Fifo => NodeType::Fifo,
            RamFSData::CharDev(_) => NodeType::CharDev,
            RamFSData::Directory(_) => NodeType::Directory,
            RamFSData::BlockDev(_) => NodeType::BlockDev,
            RamFSData::Regular(_) => NodeType::Regular,
            RamFSData::Symlink(_) => NodeType::Symlink,
            RamFSData::UnixSocket => NodeType::UnixSocket,
        }
    }
}

/// A [`RamFS`] inode.
struct RamINode {
    /// The data stored in this inode.
    data: RamFSData,
    /// Number of bytes in use excluding data structure overhead.
    size: AtomicUsize,
    /// Number of hard links to this inode.
    links: Spinlock<u16>,
    /// Inode number.
    ino: u64,
    /// Time of last access. On BadgerOS, only updated when modified or created.
    atim: AtomicTimespec,
    /// Time of last modification.
    mtim: AtomicTimespec,
    /// Time of last status change.
    ctim: AtomicTimespec,
}

/// VNode wrapper for a [`RamINode`].
struct RamVNode {
    vfs: Arc<RamFS>,
    inode: Arc<UnsafeCell<RamINode>>,
}

impl VNodeOps for RamVNode {
    fn get_blockdev(&self) -> Option<BlockDevice> {
        let inode = unsafe { self.inode.as_ref_unchecked() };
        if let RamFSData::BlockDev(dev) = &inode.data {
            Some(dev.clone())
        } else {
            None
        }
    }

    fn get_chardev(&self) -> Option<CharDevice> {
        let inode = unsafe { self.inode.as_ref_unchecked() };
        if let RamFSData::CharDev(dev) = &inode.data {
            Some(dev.clone())
        } else {
            None
        }
    }

    fn write(&self, offset: u64, wdata: &[u8]) -> EResult<()> {
        let offset: usize = offset.try_into().map_err(|_| Errno::EIO)?;
        let inode = unsafe { self.inode.as_mut_unchecked() };
        let regular = inode.data.as_regular_mut().ok_or(Errno::EINVAL)?;
        if offset.checked_add(wdata.len()).ok_or(Errno::EIO)? > regular.len() {
            return Err(Errno::EIO);
        }
        for i in 0..wdata.len() {
            regular[i + offset] = wdata[i];
        }
        Ok(())
    }

    fn read(&self, offset: u64, rdata: &mut [u8]) -> EResult<()> {
        let offset: usize = offset.try_into().map_err(|_| Errno::EIO)?;
        let inode = unsafe { self.inode.as_ref_unchecked() };
        let regular = inode.data.as_regular().ok_or(Errno::EINVAL)?;
        if offset.checked_add(rdata.len()).ok_or(Errno::EIO)? > regular.len() {
            return Err(Errno::EIO);
        }
        for i in 0..rdata.len() {
            rdata[i] = regular[i + offset];
        }
        Ok(())
    }

    fn resize(&mut self, new_size: u64) -> EResult<()> {
        let new_size: usize = new_size.try_into().map_err(|_| Errno::ENOSPC)?;
        let inode = unsafe { self.inode.as_mut_unchecked() };
        let regular = inode.data.as_regular_mut().ok_or(Errno::EINVAL)?;
        regular
            .try_reserve(new_size.saturating_sub(regular.len()))
            .map_err(|_| Errno::ENOSPC)?;
        regular.resize(new_size, 0);
        inode.size.store(new_size, Ordering::Relaxed);
        Ok(())
    }

    fn find_dirent(&self, name: &[u8]) -> EResult<Dirent> {
        let inode = unsafe { self.inode.as_ref_unchecked() };
        let directory = inode.data.as_directory().ok_or(Errno::EINVAL)?;
        directory.get(name).ok_or(Errno::ENOENT).cloned()
    }

    fn get_dirents(&self) -> EResult<Vec<Dirent>> {
        let inode = unsafe { self.inode.as_ref_unchecked() };
        let directory = inode.data.as_directory().ok_or(Errno::EINVAL)?;
        // TODO: OOM handling
        Ok(directory.values().map(Dirent::clone).collect())
    }

    fn unlink(&mut self, name: &[u8], is_rmdir: bool) -> EResult<()> {
        let inode = unsafe { self.inode.as_mut_unchecked() };
        let directory = inode.data.as_directory_mut().ok_or(Errno::EINVAL)?;

        let dirent = directory.remove(name).ok_or(Errno::ENOENT)?;
        let ino = self
            .vfs
            .inodes
            .lock_shared()
            .get(&dirent.ino)
            .unwrap()
            .clone();
        let ino = unsafe { ino.as_ref_unchecked() };

        if let RamFSData::Directory(data) = &ino.data {
            if !is_rmdir {
                return Err(Errno::EISDIR);
            } else if data.len() != 0 {
                return Err(Errno::ENOTEMPTY);
            }
        } else if is_rmdir {
            return Err(Errno::ENOTDIR);
        }

        // Pop refcount.
        assert!(unsafe { irq::disable() });
        let mut link_guard = ino.links.lock();
        let prev_links = *link_guard;
        *link_guard -= 1;
        drop(link_guard);
        unsafe { irq::enable() };

        if prev_links == 1 {
            // Last link removed.
            self.vfs.inodes.lock().remove(&dirent.ino).unwrap();
        }

        Ok(())
    }

    fn link(&mut self, name: &[u8], inode: &VNode) -> EResult<()> {
        let dir_inode = unsafe { self.inode.as_mut_unchecked() };
        let directory = dir_inode.data.as_directory_mut().ok_or(Errno::EINVAL)?;

        if directory.contains_key(name) {
            return Err(Errno::EEXIST);
        }

        let ram_inode = self
            .vfs
            .inodes
            .lock_shared()
            .get(&inode.inode)
            .cloned()
            .ok_or(Errno::EIO)?;

        let ram_inode = unsafe { ram_inode.as_mut_unchecked() };
        assert!(unsafe { irq::disable() });
        let link_res = {
            let mut links = ram_inode.links.lock();
            assert!(*links > 0);
            if *links >= u16::MAX {
                Err(Errno::EMLINK)
            } else {
                *links += 1;
                Ok(())
            }
        };
        unsafe { irq::enable() };
        link_res?;

        directory.insert(
            name.into(),
            Dirent {
                ino: ram_inode.ino,
                type_: ram_inode.data.node_type(),
                name: name.into(),
                dirent_off: 0,
            },
        );

        Ok(())
    }

    fn make_file(&mut self, name: &[u8], spec: MakeFileSpec) -> EResult<Box<dyn VNodeOps>> {
        let inode = unsafe { self.inode.as_mut_unchecked() };
        let directory = inode.data.as_directory_mut().ok_or(Errno::EINVAL)?;

        if let Some(_) = directory.get(name) {
            return Err(Errno::EEXIST);
        }

        if !self.vfs.allow_devfiles {
            match &spec {
                MakeFileSpec::CharDev(_) => return Err(Errno::EINVAL),
                MakeFileSpec::BlockDev(_) => return Err(Errno::EINVAL),
                _ => (),
            }
        }

        let data = match spec {
            MakeFileSpec::Fifo => RamFSData::Fifo,
            MakeFileSpec::CharDev(dev) => RamFSData::CharDev(dev),
            MakeFileSpec::Directory => RamFSData::Directory(BTreeMap::new()),
            MakeFileSpec::BlockDev(dev) => RamFSData::BlockDev(dev),
            MakeFileSpec::Regular => RamFSData::Regular(Vec::new()),
            MakeFileSpec::Symlink(items) => RamFSData::Symlink(items.into()),
            MakeFileSpec::UnixSocket => RamFSData::UnixSocket,
        };
        let now = Timespec::now();
        let ino = self.vfs.ino_ctr.fetch_add(1, Ordering::Relaxed);
        let new_inode = Arc::try_new(UnsafeCell::new(RamINode {
            data,
            ino,
            links: Spinlock::new(1),
            size: AtomicUsize::new(0),
            atim: AtomicTimespec::new(now),
            mtim: AtomicTimespec::new(now),
            ctim: AtomicTimespec::new(now),
        }))?;

        let ops = Box::try_new(RamVNode {
            vfs: self.vfs.clone(),
            inode: new_inode.clone(),
        })?;

        self.vfs.inodes.lock().insert(ino, new_inode.clone());

        directory.insert(
            name.into(),
            Dirent {
                ino,
                type_: ops.get_type(),
                name: name.into(),
                dirent_off: 0,
            },
        );

        Ok(Box::<dyn VNodeOps>::from(ops))
    }

    fn rename(&mut self, old_name: &[u8], new_name: &[u8]) -> EResult<()> {
        let inode = unsafe { self.inode.as_mut_unchecked() };
        let directory = inode.data.as_directory_mut().ok_or(Errno::EINVAL)?;

        if old_name == new_name {
            return Ok(());
        }

        if let Some(dirent) = directory.get(old_name) {
            directory
                .try_insert(new_name.into(), dirent.clone())
                .map_err(|_| Errno::EEXIST)?;
            directory.remove(old_name);
            Ok(())
        } else {
            Err(Errno::ENOENT)
        }
    }

    fn readlink(&self) -> EResult<Box<[u8]>> {
        let inode = unsafe { self.inode.as_ref_unchecked() };
        inode.data.as_symlink().ok_or(Errno::EINVAL).cloned()
    }

    fn stat(&self) -> EResult<Stat> {
        let inode = unsafe { self.inode.as_ref_unchecked() };
        let size = inode.size.load(Ordering::Relaxed);
        assert!(unsafe { irq::disable() });
        let nlink = *inode.links.lock_shared();
        unsafe { irq::enable() };
        Ok(Stat {
            dev: 0,
            ino: inode.ino,
            mode: NodeMode {
                type_: self.get_type(),
                others: 7,
                group: 7,
                owner: 7,
                suid: false,
                sgid: false,
                sticky: false,
            }
            .into_u16(),
            nlink,
            uid: 0,
            gid: 0,
            rdev: 0,
            size: size as u64,
            blksize: 1,
            blocks: (size / 512) as u64,
            atim: inode.atim.load(),
            mtim: inode.mtim.load(),
            ctim: inode.ctim.load(),
        })
    }

    fn get_inode(&self) -> u64 {
        let inode = unsafe { self.inode.as_ref_unchecked() };
        inode.ino
    }

    fn get_size(&self) -> u64 {
        let inode = unsafe { self.inode.as_ref_unchecked() };
        inode.size.load(Ordering::Relaxed) as u64
    }

    fn get_type(&self) -> NodeType {
        let inode = unsafe { self.inode.as_ref_unchecked() };
        inode.data.node_type()
    }

    fn sync(&self) -> EResult<()> {
        Ok(())
    }
}

/// The driver struct for [`RamFS`].
struct RamFSDriver {
    allows_devfiles: bool,
}

impl VfsDriver for RamFSDriver {
    fn detect(&self, _media: &Media) -> EResult<bool> {
        Ok(false)
    }

    fn mount(&self, media: Option<Media>, mflags: MFlags) -> EResult<Box<dyn VfsOps>> {
        if mflags & mflags::READ_ONLY != 0 {
            logkf!(
                LogLevel::Error,
                "It doesn't make sense to mount an empty RamFS as READ_ONLY"
            );
            return Err(Errno::EINVAL);
        }
        if let Some(_) = media {
            logkf!(LogLevel::Error, "RamFS does not use media");
            return Err(Errno::EINVAL);
        }
        Ok(Box::<dyn VfsOps>::from(Box::try_new(RamFS::new(
            self.allows_devfiles,
        )?)?))
    }
}

fn register_ramfs() {
    FSDRIVERS.lock().insert(
        "ramfs".into(),
        Box::new(RamFSDriver {
            allows_devfiles: false,
        }),
    );
    FSDRIVERS.lock().insert(
        "devtmpfs".into(),
        Box::new(RamFSDriver {
            allows_devfiles: true,
        }),
    );
}

register_kmodule!(ramfs, [1, 0, 0], register_ramfs);
