// SPDX-FileCopyrightText: 2025 Julian Scheffers <julian@scheffers.net>
// SPDX-FileType: SOURCE
// SPDX-License-Identifier: MIT

use core::{
    cell::UnsafeCell,
    fmt::{Debug, Write},
    panic, str,
    sync::atomic::{AtomicU32, AtomicU64},
};

use access::Access;
use alloc::{boxed::Box, collections::btree_map::BTreeMap, string::String, sync::Arc, vec::Vec};
use device::{BlockDevFile, CharDevFile};
use linkflags::LinkFlags;
use media::{Media, MediaType};
use oflags::OFlags;
use vfs::{DentCache, DentCacheDir, DentCacheType, VNode, Vfs, VfsDriver, VfsFile, mflags::MFlags};

use crate::{
    LogLevel,
    badgelib::time::Timespec,
    bindings::{
        device::{
            BaseDevice, DeviceFilters,
            class::{block::BlockDevice, char::CharDevice},
        },
        error::{EResult, Errno},
        mutex::{Mutex, SharedMutexGuard},
        raw::{errno_t, file_t},
    },
    filesystem::{
        c_api::ref_as_file,
        fifo::{Fifo, FifoShared},
    },
    logk_hexdump, logkf,
};

pub mod c_api;
pub mod device;
pub mod fatfs;
pub mod fifo;
pub mod media;
pub mod mount_root;
pub mod partition;
pub mod ramfs;
pub mod vfs;

#[repr(u32)]
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
/// Seek modes to give to [`File::seek`].
pub enum SeekMode {
    /// Set absolute position.
    Set = 0,
    /// Set relative to current position.
    Cur = 1,
    /// Set relative to end of file.
    End = 2,
}

#[rustfmt::skip]
pub mod access {
    pub type Access = u8;
    pub const READ:  u8 = 0b001;
    pub const WRITE: u8 = 0b010;
    pub const EXEC:  u8 = 0b100;
}

#[rustfmt::skip]
pub mod mode {
    pub type Mode = u16;
    /// bit mask for the file type bit field
    pub const S_IFMT:   u16 = 0o170000;
    /// socket
    pub const S_IFSOCK: u16 = 0o140000;
    /// symbolic link
    pub const S_IFLNK:  u16 = 0o120000;
    /// regular file
    pub const S_IFREG:  u16 = 0o100000;
    /// block device
    pub const S_IFBLK:  u16 = 0o060000;
    /// directory
    pub const S_IFDIR:  u16 = 0o040000;
    /// character device
    pub const S_IFCHR:  u16 = 0o020000;
    /// FIFO
    pub const S_IFIFO:  u16 = 0o010000;
}

#[derive(Clone, Copy, PartialEq, Eq)]
/// Inode mode.
pub struct NodeMode {
    pub type_: NodeType,
    pub others: Access,
    pub group: Access,
    pub owner: Access,
    pub suid: bool,
    pub sgid: bool,
    pub sticky: bool,
}

impl NodeMode {
    /// Convert into the format for the posix `struct stat` `st_mode` field.
    pub const fn into_u16(self) -> u16 {
        let mode = match self.type_ {
            NodeType::Unknown => 0,
            NodeType::Fifo => mode::S_IFIFO,
            NodeType::CharDev => mode::S_IFCHR,
            NodeType::Directory => mode::S_IFDIR,
            NodeType::BlockDev => mode::S_IFBLK,
            NodeType::Regular => mode::S_IFREG,
            NodeType::Symlink => mode::S_IFLNK,
            NodeType::UnixSocket => mode::S_IFSOCK,
        };
        mode + (self.others as u16) * 0o0001
            + (self.group as u16) * 0o0010
            + (self.owner as u16) * 0o0100
            + self.suid as u16 * 0o4000
            + self.sgid as u16 * 0o2000
            + self.sticky as u16 * 0o1000
    }

    /// Convert from the format for the posix `struct stat` `st_mode` field.
    pub const fn from_u16(value: u16) -> Self {
        let type_ = match value & mode::S_IFMT {
            mode::S_IFIFO => NodeType::Fifo,
            mode::S_IFCHR => NodeType::CharDev,
            mode::S_IFDIR => NodeType::Directory,
            mode::S_IFBLK => NodeType::BlockDev,
            mode::S_IFREG => NodeType::Regular,
            mode::S_IFLNK => NodeType::Symlink,
            mode::S_IFSOCK => NodeType::UnixSocket,
            _ => NodeType::Unknown,
        };
        Self {
            type_,
            others: (value & 0o007) as u8,
            group: ((value & 0o070) >> 3) as u8,
            owner: ((value & 0o700) >> 3) as u8,
            suid: value & 0o4000 != 0,
            sgid: value & 0o2000 != 0,
            sticky: value & 0o1000 != 0,
        }
    }
}

impl Into<u16> for NodeMode {
    fn into(self) -> u16 {
        self.into_u16()
    }
}

impl From<u16> for NodeMode {
    fn from(value: u16) -> Self {
        Self::from_u16(value)
    }
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
/// Inode statistics obtained from [`File::stat`].
pub struct Stat {
    /// ID and class of device containing file.
    pub dev: u64,
    /// Inode number.
    pub ino: u64,
    /// File type and mode flags
    pub mode: u16,
    /// Number of hard links.
    pub nlink: u16,
    /// Owner user ID.
    pub uid: u16,
    /// Owner group ID.
    pub gid: u16,
    /// ID of device for device special files.
    pub rdev: u64,
    /// Byte size of this file.
    pub size: u64,
    /// Block size for filesystem I/O.
    pub blksize: u64,
    /// Number of 512 byte blocks allocated (represents actual used disk space).
    pub blocks: u64,
    /// Time of last access. On BadgerOS, only updated when modified or created.
    pub atim: Timespec,
    /// Time of last modification.
    pub mtim: Timespec,
    /// Time of last status change.
    pub ctim: Timespec,
}

#[repr(u32)]
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Default, Debug)]
/// Types of file recognised by [`Dirent`].
pub enum NodeType {
    #[default]
    /// Unknown file type.
    Unknown,
    /// Named pipe.
    Fifo,
    /// Character device.
    CharDev,
    /// Directory.
    Directory,
    /// Block device.
    BlockDev,
    /// Regular file.
    Regular,
    /// Symbolic link.
    Symlink,
    /// UNIX domain socket.
    UnixSocket,
}

#[derive(Clone, Default)]
/// An abstract directory entry obtained from [`VNodeOps::find_dirent`].
pub struct Dirent {
    /// Inode number.
    pub ino: u64,
    /// Type of entry this is.
    pub type_: NodeType,
    /// File name:
    pub name: Vec<u8>,
    /// On-disk position of the dirent.
    pub dirent_off: u64,
}

impl Debug for Dirent {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        f.debug_struct("Dirent")
            .field("ino", &self.ino)
            .field("type_", &self.type_)
            .field("name", &{
                struct ByteStr<'a>(&'a [u8]);
                impl<'a> core::fmt::Debug for ByteStr<'a> {
                    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
                        f.write_str("b\"")?;
                        for &b in self.0 {
                            match b {
                                b'\\' => f.write_str("\\\\")?,
                                b'"' => f.write_str("\\\"")?,
                                0x20..=0x7E => f.write_char(b as char)?,
                                _ => write!(f, "\\x{:02x}", b)?,
                            }
                        }
                        f.write_str("\"")
                    }
                }
                ByteStr(&self.name)
            })
            .field("dirent_off", &self.dirent_off)
            .finish()
    }
}

/// Handle to an open file. Dropping it closes the file.
pub trait File: Sync {
    /// Get all entries in this directory.
    fn get_dirents(&self) -> EResult<Vec<Dirent>> {
        let vnode = self.get_vnode().ok_or(Errno::ESPIPE)?;
        let ops = vnode.ops.lock_shared();
        ops.get_dirents(&vnode)
    }
    /// Get the device that this file represents, if any.
    fn get_device(&self) -> Option<BaseDevice> {
        None
    }
    /// Get the stat info for this file's inode.
    fn stat(&self) -> EResult<Stat>;
    /// Get the position in the file.
    fn tell(&self) -> EResult<u64>;
    /// Change the position in the file.
    fn seek(&self, mode: SeekMode, offset: i64) -> EResult<u64>;
    /// Write bytes to this file.
    fn write(&self, wdata: &[u8]) -> EResult<usize>;
    /// Read bytes from this file.
    fn read(&self, rdata: &mut [u8]) -> EResult<usize>;
    /// Resize the file to a new length.
    fn resize(&self, size: u64) -> EResult<()>;
    /// Sync the underlying caches to disk.
    fn sync(&self) -> EResult<()>;
    /// Get the underlying vnode (if it exists).
    fn get_vnode(&self) -> Option<Arc<VNode>>;
}

#[derive(Clone)]
/// Specifies how a file is to be created.
pub enum MakeFileSpec<'a> {
    /// Named pipe.
    Fifo,
    /// Character device.
    CharDev(CharDevice),
    /// Directory.
    Directory,
    /// Block device.
    BlockDev(BlockDevice),
    /// Regular file.
    Regular,
    /// Symbolic link.
    Symlink(&'a [u8]),
    /// UNIX domain socket.
    UnixSocket,
}

impl MakeFileSpec<'_> {
    pub fn node_type(&self) -> NodeType {
        match self {
            MakeFileSpec::Fifo => NodeType::Fifo,
            MakeFileSpec::CharDev(_) => NodeType::CharDev,
            MakeFileSpec::Directory => NodeType::Directory,
            MakeFileSpec::BlockDev(_) => NodeType::BlockDev,
            MakeFileSpec::Regular => NodeType::Regular,
            MakeFileSpec::Symlink(_) => NodeType::Symlink,
            MakeFileSpec::UnixSocket => NodeType::UnixSocket,
        }
    }
}

#[rustfmt::skip]
pub mod oflags {
    /// Type to use for file opening flags.
    pub type OFlags = u32;
    /// Allows for reading the file.
    pub const READ_ONLY:  u32 = 0x0000_0001;
    /// Allows for writing the file.
    pub const WRITE_ONLY: u32 = 0x0000_0002;
    /// Allows for both reading and writing.
    pub const READ_WRITE: u32 = 0x0000_0003;
    /// Makes writing work in append mode.
    pub const APPEND:     u32 = 0x0000_0004;
    /// Fail if the target is a directory.
    pub const FILE_ONLY:  u32 = 0x0000_0008;
    /// Fail if the target is not a directory.
    pub const DIR_ONLY:   u32 = 0x0000_0010;
    /// Do not follow the last symlink.
    pub const NOFOLLOW:   u32 = 0x0000_0020;
    /// Create the file if it does not exist.
    pub const CREATE:     u32 = 0x0000_0040;
    /// Fail if the file exists already.
    pub const EXCLUSIVE:  u32 = 0x0000_0080;
    /// Truncate the file on open.
    pub const TRUNCATE:   u32 = 0x0000_0100;
    /// Use non-blocking I/O.
    pub const NONBLOCK:   u32 = 0x0000_0200;
}

#[rustfmt::skip]
pub mod linkflags {
    pub type LinkFlags = u32;
    /// Follow symlinks for old path on [`super::link`] and [`super::rename`].
    pub const FOLLOW_LINKS: u32 = 0x0000_0001;
}

/// The maximum number of symlinks followed.
pub const LINK_MAX: usize = 32;
/// The maximum path length.
pub const PATH_MAX: usize = 4096;
/// The maximum filename length.
pub const NAME_MAX: usize = 255;

/// The currently mounted root filesystem.
static ROOT_FS: Mutex<Option<Arc<Vfs>>> = unsafe { Mutex::new_static(None) };

/// Table of mounted filesystems.
static MOUNT_TABLE: Mutex<BTreeMap<Box<[u8]>, Arc<Vfs>>> =
    unsafe { Mutex::new_static(BTreeMap::new()) };

/// Table of filesystem drivers.
pub static FSDRIVERS: Mutex<BTreeMap<String, Box<dyn VfsDriver>>> =
    unsafe { Mutex::new_static(BTreeMap::new()) };

/// Helper function that gets the root directory handle.
fn root_vnode() -> EResult<Arc<VNode>> {
    if let Some(fs) = &*ROOT_FS.lock_shared() {
        Ok(fs.root())
    } else {
        logkf!(
            LogLevel::Warning,
            "Filesystem op run without a filesystem mounted"
        );
        Err(Errno::EAGAIN)
    }
}

/// Helper function that gets the VNode for `at` parameters.
fn at_vnode(at: Option<&dyn File>) -> EResult<Arc<VNode>> {
    match at {
        Some(x) => x.get_vnode().ok_or(Errno::ENOTDIR),
        None => root_vnode(),
    }
}

/// Walk down the filesystem to a certain path.
fn walk(mut at: Arc<DentCache>, path: &[u8], follow_last_symlink: bool) -> EResult<Arc<DentCache>> {
    if path.len() > PATH_MAX {
        // There is no distinction between the errno for NAME_MAX exceeded or PATH_MAX exceeded.
        return Err(Errno::ENAMETOOLONG);
    }

    /// A string slice or an arc of dentcache.
    enum LinkValue<'a> {
        None,
        Slice(&'a [u8]),
        Dent(Arc<DentCache>),
    }
    /// An entry on the symlink stack.
    struct LinkEntry<'a> {
        name: LinkValue<'a>,
        offset: usize,
    }
    impl LinkEntry<'_> {
        fn name(&self) -> &[u8] {
            match &self.name {
                LinkValue::None => panic!(),
                LinkValue::Slice(x) => x,
                LinkValue::Dent(x) => x.readlink().unwrap(),
            }
        }
    }

    // The path stack.
    let mut stack = [const {
        LinkEntry {
            name: LinkValue::None,
            offset: 0,
        }
    }; LINK_MAX];
    let mut depth = 0usize;
    let mut links_passed = 0usize;
    stack[0].name = LinkValue::Slice(path);

    // If the path starts with `/`, set `at` to root.
    if path.len() == 0 {
        return Err(Errno::ENOENT);
    } else if path[0] == b'/' {
        at = root_vnode()?.dentcache.clone().unwrap();
    }

    loop {
        if stack[depth].offset >= stack[depth].name().len() {
            // Pop empty names.
            if depth > 0 {
                depth -= 1;
                continue;
            } else {
                break;
            }
        } else if stack[depth].name()[stack[depth].offset] == b'/' {
            // Skip forward slashes.
            match &at.type_ {
                DentCacheType::Negative => return Err(Errno::ENOENT),
                DentCacheType::Directory(_) => (),
                _ => return Err(Errno::ENOTDIR),
            }
            stack[depth].offset += 1;
            continue;
        }

        // Find next forward slash in the name.
        let name = stack[depth].name();
        let offset = stack[depth].offset;
        let component_len = name[offset..name.len()]
            .iter()
            .position(|x| *x == b'/')
            .unwrap_or(name.len() - offset);
        if component_len > NAME_MAX {
            return Err(Errno::ENAMETOOLONG);
        }

        // Get next component.
        let next = at.lookup(&name[offset..offset + component_len])?;
        stack[depth].offset += component_len;

        let has_more_path = depth > 0 || stack[depth].offset < stack[depth].name().len();
        match &next.type_ {
            DentCacheType::Negative if has_more_path => return Err(Errno::ENOENT),
            DentCacheType::Symlink(_) => {
                if follow_last_symlink || has_more_path {
                    // Follow the symlink.
                    if links_passed >= LINK_MAX {
                        return Err(Errno::EMLINK);
                    }
                    links_passed += 1;
                    depth += 1;
                    stack[depth] = LinkEntry {
                        name: LinkValue::Dent(at.clone()),
                        offset: 0,
                    };
                    if at.readlink()?.len() == 0 {
                        return Err(Errno::ENOENT);
                    } else if at.readlink()?[0] == b'/' {
                        at = root_vnode()?.dentcache.clone().unwrap();
                    } else {
                        at = next;
                    }
                }
            }
            _ => at = next,
        }
    }

    Ok(at)
}

/// Helper function for [`oflags::CREATE`] logic in [`open`].
fn o_creat_helper(to_create: Arc<DentCache>, exclusive: bool) -> EResult<Arc<VNode>> {
    let dir_cache = to_create.parent.clone().unwrap();

    let mut guard = dir_cache.type_.as_dir().unwrap().lock();

    // Check whether the file was created in the mean time.
    if let Some(weak) = guard.children.get(&*to_create.dirent.name) {
        if let Some(arc) = weak.upgrade() {
            match &arc.type_ {
                DentCacheType::Negative => (),
                DentCacheType::Directory(_) => return Err(Errno::EISDIR),
                DentCacheType::Symlink(_) => {
                    logkf!(
                        LogLevel::Warning,
                        "TODO: Determine sematics for symlink created in oflags::CREATE race condition"
                    );
                    return Err(Errno::EAGAIN);
                }
                DentCacheType::File => {
                    if exclusive {
                        return Err(Errno::EEXIST);
                    } else {
                        return arc.open_vnode();
                    }
                }
            }
        }
    }

    // A new regular file is to be created.
    let dir_vnode = dir_cache.open_vnode()?;
    let mut dir_ops = dir_vnode.ops.lock();
    let new_ops = dir_ops.make_file(&dir_vnode, &to_create.dirent.name, MakeFileSpec::Regular)?;
    let inode = new_ops.get_inode();
    let new_vnode = Arc::try_new(VNode {
        ops: Mutex::new(new_ops),
        ino: inode,
        vfs: dir_vnode.vfs.clone(),
        dentcache: None,
        flags: AtomicU32::new(0),
        type_: NodeType::Regular,
        fifo: None,
    })?;

    // Successfully created new VNode.
    dir_vnode
        .vfs
        .vnodes
        .lock()
        .insert(inode, Arc::downgrade(&new_vnode));

    // TODO: This could be optimized by replacing the cache entry with a regular file version.
    guard.children.remove(&*to_create.dirent.name);

    Ok(new_vnode)
}

/// Open a file.
pub fn open(at: Option<&dyn File>, path: &[u8], oflags: OFlags) -> EResult<Arc<dyn File>> {
    // Validate oflags.
    if oflags & oflags::DIR_ONLY != 0
        && oflags
            & (oflags::CREATE
                | oflags::EXCLUSIVE
                | oflags::FILE_ONLY
                | oflags::WRITE_ONLY
                | oflags::APPEND
                | oflags::TRUNCATE)
            != 0
    {
        // A flag incompatible with DIR_ONLY was passed.
        return Err(Errno::EINVAL);
    } else if oflags & oflags::APPEND != 0 && oflags & oflags::WRITE_ONLY == 0 {
        // Append requires write.
        return Err(Errno::EINVAL);
    } else if oflags & oflags::READ_WRITE == 0 {
        // Neither read nor write requested.
        return Err(Errno::EINVAL);
    } else if oflags & oflags::EXCLUSIVE != 0 && oflags & oflags::CREATE == 0 {
        // Exclusive without create can never succeed.
        return Err(Errno::EINVAL);
    }

    // Find target file.
    let at = at_vnode(at)?;
    let cache = walk(
        at.dentcache.clone().ok_or(Errno::ENOTDIR)?,
        path,
        oflags & oflags::NOFOLLOW == 0,
    )?;

    // Open the target VNode.
    let vnode = match &cache.type_ {
        DentCacheType::Negative => {
            if oflags & oflags::CREATE == 0 {
                return Err(Errno::ENOENT);
            }
            o_creat_helper(cache.clone(), oflags & oflags::EXCLUSIVE != 0)?
        }
        DentCacheType::Directory(_) => {
            if oflags & oflags::EXCLUSIVE != 0 {
                return Err(Errno::EEXIST);
            } else if oflags & oflags::FILE_ONLY != 0 {
                return Err(Errno::EISDIR);
            } else {
                cache.open_vnode()?
            }
        }
        _ => {
            if oflags & oflags::DIR_ONLY != 0 {
                return Err(Errno::ENOTDIR);
            } else {
                cache.open_vnode()?
            }
        }
    };

    match vnode.type_ {
        NodeType::Fifo => {
            // FIFO file ops.
            Ok(Box::<dyn File>::from(Box::try_new(Fifo {
                vnode: Some(vnode.clone()),
                is_nonblock: oflags & oflags::NONBLOCK != 0,
                allow_read: oflags & oflags::READ_ONLY != 0,
                allow_write: oflags & oflags::WRITE_ONLY != 0,
                shared: vnode.fifo.clone().unwrap(),
            })?)
            .into())
        }
        NodeType::CharDev => {
            // Character device file ops.
            Ok(Box::<dyn File>::from(Box::try_new(CharDevFile::new(vnode.clone()))?).into())
        }
        NodeType::BlockDev => {
            // Block device file ops.
            Ok(Box::<dyn File>::from(Box::try_new(BlockDevFile::new(
                vnode.clone(),
                oflags & oflags::READ_ONLY != 0,
                oflags & oflags::WRITE_ONLY != 0,
            ))?)
            .into())
        }
        NodeType::UnixSocket => todo!("UNIX domain socket file ops"),
        _ => {
            // Regular file ops.
            Ok(Box::<dyn File>::from(Box::try_new(VfsFile {
                vnode,
                offset: AtomicU64::new(0),
                is_append: oflags & oflags::APPEND != 0,
                allow_read: oflags & oflags::READ_ONLY != 0,
                allow_write: oflags & oflags::WRITE_ONLY != 0,
            })?)
            .into())
        }
    }
}

/// Create a new name for a file.
pub fn link(
    old_at: Option<&dyn File>,
    old_path: &[u8],
    new_at: Option<&dyn File>,
    new_path: &[u8],
    flags: LinkFlags,
) -> EResult<()> {
    // Find source and destination.
    let old_at = at_vnode(old_at)?;
    let new_at = at_vnode(new_at)?;
    let old = walk(
        old_at.dentcache.clone().ok_or(Errno::ENOTDIR)?,
        old_path,
        flags & linkflags::FOLLOW_LINKS != 0,
    )?;
    let new = walk(
        new_at.dentcache.clone().ok_or(Errno::ENOTDIR)?,
        new_path,
        flags & linkflags::FOLLOW_LINKS != 0,
    )?;

    if old.type_.as_dir().is_some() {
        return Err(Errno::EISDIR);
    }

    // Get parent dirent caches.
    let old_dir_cache = old.parent.clone().ok_or(Errno::EISDIR)?;
    let new_dir_cache = old.parent.clone().ok_or(Errno::EISDIR)?;

    let old_guard = old_dir_cache.type_.as_dir().unwrap().lock();
    let new_guard = (!Arc::ptr_eq(&old_dir_cache, &new_dir_cache))
        .then(|| new_dir_cache.type_.as_dir().unwrap().lock());

    // Perform link operation on VFS.
    let dir_vnode = new_dir_cache.open_vnode()?;
    dir_vnode
        .ops
        .lock()
        .link(&dir_vnode, &new.dirent.name, &*old.open_vnode()?)?;

    // Invalidate dentcache for new.
    new_guard
        .unwrap_or(old_guard)
        .children
        .remove(&*new.dirent.name);

    Ok(())
}

/// Remove a file or directory.
/// Uses POSIX `rmdir` semantics iff `is_rmdir`, otherwise POSIX unlink semantics.
pub fn unlink(at: Option<&dyn File>, path: &[u8], is_rmdir: bool) -> EResult<()> {
    // Find target file.
    let at = at_vnode(at)?;
    let to_remove = walk(at.dentcache.clone().ok_or(Errno::ENOTDIR)?, path, false)?;

    // Get parent dirent cache.
    let dir_cache = to_remove.parent.clone().ok_or(
        // If no parent, this is the root directory of a VFS.
        if is_rmdir {
            Errno::ENOTEMPTY
        } else {
            Errno::EISDIR
        },
    )?;
    let mut guard = dir_cache.type_.as_dir().unwrap().lock();

    // If the target is a dir, lock it so it cannot be concurrently modified.
    let _target_guard = to_remove.type_.as_dir().map(Mutex::lock);

    // Unlink the node.
    let dir_vnode = dir_cache.open_vnode()?;
    dir_vnode
        .ops
        .lock()
        .unlink(&dir_vnode, &to_remove.dirent.name, is_rmdir)?;

    // Delete the dirent cache entry.
    guard.children.remove(&*to_remove.dirent.name);

    Ok(())
}

/// Create a new file or directory.
pub fn make_file(at: Option<&dyn File>, path: &[u8], spec: MakeFileSpec) -> EResult<()> {
    // Find target file.
    let at = at_vnode(at)?;
    let to_create = walk(at.dentcache.clone().ok_or(Errno::ENOTDIR)?, path, false)?;

    let dir_cache = to_create.parent.clone().ok_or(Errno::EEXIST)?;
    let mut guard = dir_cache.type_.as_dir().unwrap().lock();

    // Check whether the file already exists.
    if let Some(weak) = guard.children.get(&*to_create.dirent.name) {
        if let Some(arc) = weak.upgrade() {
            match &arc.type_ {
                DentCacheType::Negative => (),
                _ => return Err(Errno::EEXIST),
            }
        }
    }

    // A new file is to be created.
    let dir_vnode = dir_cache.open_vnode()?;
    let mut dir_ops = dir_vnode.ops.lock();
    let type_ = spec.node_type();
    let new_ops = dir_ops.make_file(&dir_vnode, &to_create.dirent.name, spec)?;
    let inode = new_ops.get_inode();
    let fifo = (type_ == NodeType::Fifo).then(|| FifoShared::new());
    let new_vnode = Arc::try_new(VNode {
        ops: Mutex::new(new_ops),
        ino: inode,
        vfs: dir_vnode.vfs.clone(),
        dentcache: None,
        flags: AtomicU32::new(0),
        type_,
        fifo,
    })?;

    // Successfully created new VNode.
    dir_vnode
        .vfs
        .vnodes
        .lock()
        .insert(inode, Arc::downgrade(&new_vnode));

    // TODO: This could be optimized by replacing the cache entry.
    guard.children.remove(&*to_create.dirent.name);

    Ok(())
}

/// Rename a file within the same filesystem.
pub fn rename(
    old_at: Option<&dyn File>,
    old_path: &[u8],
    new_at: Option<&dyn File>,
    new_path: &[u8],
    flags: LinkFlags,
) -> EResult<()> {
    // Find source and destination.
    let old_at = at_vnode(old_at)?;
    let new_at = at_vnode(new_at)?;
    let old = walk(
        old_at.dentcache.clone().ok_or(Errno::ENOTDIR)?,
        old_path,
        flags & linkflags::FOLLOW_LINKS != 0,
    )?;
    let new = walk(
        new_at.dentcache.clone().ok_or(Errno::ENOTDIR)?,
        new_path,
        flags & linkflags::FOLLOW_LINKS != 0,
    )?;

    // Get parent dirent caches.
    let old_dir_cache = old.parent.clone().ok_or(Errno::EISDIR)?;
    let new_dir_cache = old.parent.clone().ok_or(Errno::EISDIR)?;
    let old_dir_vnode = old_dir_cache.open_vnode()?;

    let mut old_guard = old_dir_cache.type_.as_dir().unwrap().lock();
    let new_guard = if Arc::ptr_eq(&old_dir_cache, &new_dir_cache) {
        // Rename within directory.
        old_dir_vnode
            .ops
            .lock()
            .rename(&old_dir_vnode, &old.dirent.name, &new.dirent.name)?;
        None
    } else {
        // Rename across directories.
        let guard = new_dir_cache.type_.as_dir().unwrap().lock();
        if !Arc::ptr_eq(&old_dir_cache.vfs, &new_dir_cache.vfs) {
            return Err(Errno::EXDEV);
        }
        let new_dir_vnode = new_dir_cache.open_vnode()?;
        old_dir_cache.vfs.ops.lock_shared().rename(
            &old_dir_cache.vfs,
            &old_dir_vnode,
            &old_dir_cache.dirent.name,
            &new_dir_vnode,
            &new_dir_cache.dirent.name,
        )?;
        Some(guard)
    };

    // Invalidate cache entries.
    old_guard.children.remove(&*old.dirent.name).unwrap();
    new_guard
        .unwrap_or(old_guard)
        .children
        .remove(&*new.dirent.name);

    Ok(())
}

/// Get the real path from some canonical path.
pub fn realpath(at: Option<&dyn File>, path: &[u8], follow_last_symlink: bool) -> EResult<Vec<u8>> {
    let at = at_vnode(at)?;
    let cache = walk(
        at.dentcache.clone().ok_or(Errno::ENOTDIR)?,
        path,
        follow_last_symlink,
    )?;
    cache.realpath()
}

/// Detect the filesystem on a medium.
fn detect<'a>(
    media: &Media,
    drivers: &'a BTreeMap<String, Box<dyn VfsDriver>>,
) -> EResult<&'a str> {
    todo!()
}

/// Helper function that prepares a standalone [`Vfs`] to be used by [`mount`].
fn create_vfs(
    drivers: &SharedMutexGuard<'_, BTreeMap<String, Box<dyn VfsDriver>>>,
    mountpoint: Option<Arc<VNode>>,
    type_: &str,
    media: Option<Media>,
    mflags: MFlags,
) -> EResult<Arc<Vfs>> {
    let driver = if let Some(x) = drivers.get(type_) {
        x
    } else {
        logkf!(LogLevel::Error, "No such filesystem driver: {}", type_);
        return Err(Errno::ENOTSUP);
    };

    let vfs_ops = driver.mount(media, mflags)?;

    let vfs = Arc::try_new(Vfs {
        ops: Mutex::new(vfs_ops),
        vnodes: Mutex::new(BTreeMap::new()),
        root: UnsafeCell::new(None),
        mountpoint,
        flags: AtomicU32::new(0),
    })
    .unwrap();

    let root_ops = vfs.ops.lock_shared().open_root(&vfs)?;
    let root_ino = root_ops.get_inode();

    let dentcache = DentCache {
        type_: DentCacheType::Directory(Mutex::new(DentCacheDir::EMPTY)),
        vfs: vfs.clone(),
        parent: None,
        vnode: Mutex::new(None),
        dirent: Dirent {
            ino: root_ino,
            type_: NodeType::Directory,
            name: b"/".into(),
            dirent_off: 0,
        },
    };

    let root = Arc::new(VNode {
        ops: Mutex::new(root_ops),
        ino: root_ino,
        vfs: vfs.clone(),
        dentcache: Some(Arc::new(dentcache)),
        flags: AtomicU32::new(0),
        type_: NodeType::Directory,
        fifo: None,
    });
    vfs.vnodes.lock().insert(root.ino, Arc::downgrade(&root));
    unsafe { *vfs.root.as_mut_unchecked() = Some(root) };

    Ok(vfs)
}

/// Mount a new filesystem.
pub fn mount(
    at: Option<&dyn File>,
    path: &[u8],
    type_: Option<&str>,
    media: Option<Media>,
    mflags: MFlags,
) -> EResult<()> {
    // Determine filesystem type.
    let drivers = FSDRIVERS.lock_shared();
    let type_ = if let Some(x) = type_ {
        x
    } else if let Some(media) = &media {
        detect(media, &drivers)?
    } else {
        logkf!(LogLevel::Error, "Neither type nor media specified to mount");
        return Err(Errno::EINVAL);
    };

    // Lock mounts table while other mounting logic runs.
    let mut mounts = MOUNT_TABLE.lock();

    // If the mounts table is empty (there is no root VFS), this must be mounted at `/`.
    if mounts.len() == 0 {
        if path != b"/" {
            logkf!(LogLevel::Error, "/ needs to be mounted first");
            return Err(Errno::ENOENT);
        }
        let vfs = create_vfs(&drivers, None, type_, media, mflags)?;
        mounts.insert((*b"/").into(), vfs.clone());
        *ROOT_FS.lock() = Some(vfs);
        return Ok(());
    }

    // Get the directory that is requested for the mountpoint.
    let orig_at = at;
    let at = at_vnode(at)?;
    let cache = walk(at.dentcache.clone().ok_or(Errno::ENOTDIR)?, path, true)?.follow_mounts();
    // Lock it so no modifications can happen while mounting there.
    let cache_dir = cache.type_.as_dir().ok_or(Errno::ENOTDIR)?;
    let mut cache_guard = cache_dir.lock();

    if cache_guard.children.len() != 0 {
        // Mountpoint must be empty.
        logkf!(LogLevel::Error, "Mountpoint root isn't empty");
        return Err(Errno::ENOTEMPTY);
    } else if cache.is_vfs_root() {
        // Cannot stack mounts.
        logkf!(LogLevel::Warning, "Stacked mounts are not supported yet");
        return Err(Errno::ENOTSUP);
    }

    // Create and insert VFS.
    let vfs = create_vfs(&drivers, Some(cache.open_vnode()?), type_, media, mflags)?;
    mounts.insert(cache.realpath()?.into(), vfs.clone());
    cache_guard.mounted = Some(vfs);

    drop(cache_guard);
    drop(mounts);
    drop(drivers);

    // Notify device subsystem.
    unsafe extern "C" {
        fn device_devtmpfs_mounted(devtmpfs_root: file_t) -> errno_t;
    }
    if let EResult::Err(x) = try {
        let vfs_root_dir = open(orig_at, path, oflags::DIR_ONLY | oflags::READ_ONLY)?;
        Errno::check(unsafe { device_devtmpfs_mounted(ref_as_file(&*vfs_root_dir)) })?;
    } {
        logkf!(LogLevel::Warning, "Failed to populate devtmpfs: {}", x);
    }

    Ok(())
}

/// Create an unnamed pipe.
/// The end written to is 0, the end read from is 1.
pub fn pipe(oflags: OFlags) -> EResult<(Arc<dyn File>, Arc<dyn File>)> {
    // TODO: OOM handling.
    let shared = FifoShared::new();
    shared.open(true, true, true);
    let write_end = Arc::new(Fifo {
        vnode: None,
        is_nonblock: (oflags & oflags::NONBLOCK) != 0,
        allow_read: false,
        allow_write: true,
        shared: shared.clone(),
    });
    let read_end = Arc::new(Fifo {
        vnode: None,
        is_nonblock: (oflags & oflags::NONBLOCK) != 0,
        allow_read: true,
        allow_write: false,
        shared,
    });
    Ok((write_end, read_end))
}

#[unsafe(no_mangle)]
unsafe extern "C" fn fatfs_test() {
    let devs = BlockDevice::filter(DeviceFilters::default()).unwrap();

    // We assume the FAT FS to be at 4096 * 512 offset in the first block device.
    let dev = devs.iter().next().unwrap().clone();
    let media = Media {
        offset: 4096 * 512,
        size: 8192 * 512,
        storage: MediaType::Block(dev),
    };

    // Mount the FAT filesystem.
    mount(None, b"/", Some("vfat"), Some(media), 0).unwrap();

    // Print contents of root dir.
    let dirents = open(None, b"/", oflags::READ_ONLY)
        .unwrap()
        .get_dirents()
        .unwrap();
    logkf!(LogLevel::Debug, "Dirents: {:#?}", &dirents);

    // Try printing the contents of a file.
    let file = open(None, b"/.file", oflags::READ_ONLY).unwrap();
    let mut buf = [0u8; 1024];
    let read = file.read(&mut buf).unwrap();
    assert!(read > 0);
    logk_hexdump(
        LogLevel::Debug,
        "First bytes in the file:",
        Some(0),
        &buf[..read],
    );

    // Print the end of bigfile.
    let file = open(None, b"/BIGFILE", oflags::READ_WRITE).unwrap();
    let mut buf = [0u8; 1024];
    let pos = file.seek(SeekMode::End, -1024).unwrap();
    let read = file.read(&mut buf).unwrap();
    assert!(read > 0);
    logk_hexdump(
        LogLevel::Debug,
        "Last bytes in the file:",
        Some(pos as usize),
        &buf[..read],
    );

    // Write the the bigfile.
    file.seek(SeekMode::Set, 0).unwrap();
    file.write(b"This data brought to you by overwriting the beginning of the file.\n\n")
        .unwrap();
}
