// SPDX-FileCopyrightText: 2025 Julian Scheffers <julian@scheffers.net>
// SPDX-FileType: SOURCE
// SPDX-License-Identifier: MIT

use core::{
    cell::{OnceCell, UnsafeCell},
    panic, str,
    sync::atomic::{AtomicU32, AtomicU64},
};

use access::Access;
use alloc::{boxed::Box, collections::btree_map::BTreeMap, sync::Arc, vec::Vec};
use oflags::OFlags;
use ramfs::RamFS;
use vfs::{DentCache, DentCacheDir, DentCacheType, VNode, Vfs, VfsFile, VfsOps};

use crate::{
    LogLevel,
    bindings::{
        error::{EResult, Errno},
        mutex::Mutex,
    },
    time::Timespec,
};

pub mod fifo;
pub mod mount_root;
pub mod partition;
pub mod ramfs;
pub mod vfs;

#[repr(u32)]
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
/// Seek modes to give to [`File::seek`].
pub enum SeekMode {
    /// Set absolute position.
    Set,
    /// Set relative to current position.
    Cur,
    /// Set relative to end of file.
    End,
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
        mode + (self.others as u16)
            + (self.group as u16) * 0o10
            + (self.owner as u16) * 0o100
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
    /// ID of device containing file.
    pub dev: u32,
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
    pub rdev: u32,
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
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Default)]
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

/// Handle to an open file. Dropping it closes the file.
pub trait File: Sync {
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
pub enum NewFileSpec {
    /// Named pipe.
    Fifo,
    /// Character device.
    CharDev, // TODO: Determine appropriate enum payload.
    /// Directory.
    Directory,
    /// Block device.
    BlockDev, // TODO: Determine appropriate enum payload.
    /// Regular file.
    Regular,
    /// Symbolic link.
    Symlink(Vec<u8>),
    /// UNIX domain socket.
    UnixSocket,
}

/// Represents some unresolved file path.
pub trait PathSpec {
    /// Get the vnode to be relative to and the path buffer to use.
    fn path_spec(&self) -> EResult<(Arc<VNode>, &[u8])>;
}

impl PathSpec for &str {
    fn path_spec(&self) -> EResult<(Arc<VNode>, &[u8])> {
        Ok((root_vnode()?, self.as_bytes()))
    }
}

impl PathSpec for &[u8] {
    fn path_spec(&self) -> EResult<(Arc<VNode>, &[u8])> {
        Ok((root_vnode()?, self))
    }
}

impl PathSpec for (&dyn File, &str) {
    fn path_spec(&self) -> EResult<(Arc<VNode>, &[u8])> {
        Ok((
            self.0
                .get_vnode()
                .map(|f| Ok(f))
                .unwrap_or_else(root_vnode)?,
            self.1.as_bytes(),
        ))
    }
}

impl PathSpec for (&dyn File, &[u8]) {
    fn path_spec(&self) -> EResult<(Arc<VNode>, &[u8])> {
        Ok((
            self.0
                .get_vnode()
                .map(|f| Ok(f))
                .unwrap_or_else(root_vnode)?,
            self.1,
        ))
    }
}

impl PathSpec for (Arc<VNode>, &str) {
    fn path_spec(&self) -> EResult<(Arc<VNode>, &[u8])> {
        Ok((self.0.clone(), self.1.as_bytes()))
    }
}

impl PathSpec for (Arc<VNode>, &[u8]) {
    fn path_spec(&self) -> EResult<(Arc<VNode>, &[u8])> {
        Ok((self.0.clone(), self.1))
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
}

/// The maximum number of symlinks followed.
pub const LINK_MAX: usize = 32;
/// The currently mounted root filesystem.
static ROOT_FS: Mutex<Option<Arc<Vfs>>> = unsafe { Mutex::new_static(None) };
/// Table of mounted filesystems.
static MOUNT_TABLE: Mutex<BTreeMap<Box<[u8]>, Arc<Vfs>>> =
    unsafe { Mutex::new_static(BTreeMap::new()) };

/// Helper function that gets thr root directory handle.
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

/// Walk down the filesystem to a certain path.
fn walk(mut at: Arc<DentCache>, path: &[u8], follow_last_symlink: bool) -> EResult<Arc<DentCache>> {
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
    let new_ops = dir_ops.make_file(&to_create.dirent.name, NewFileSpec::Regular)?;
    let inode = new_ops.get_inode();
    let new_vnode = Arc::try_new(VNode {
        ops: Mutex::new(new_ops),
        inode,
        vfs: dir_vnode.vfs.clone(),
        dentcache: None,
        flags: AtomicU32::new(0),
        type_: NodeType::Regular,
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
pub fn open(path: &dyn PathSpec, oflags: OFlags) -> EResult<Arc<dyn File>> {
    // Validate oflags.
    if oflags & oflags::DIR_ONLY != 0
        && oflags
            & (oflags::CREATE
                | oflags::EXCLUSIVE
                | oflags::FILE_ONLY
                | oflags::WRITE_ONLY
                | oflags::APPEND)
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
    let (at, path) = path.path_spec()?;
    let cache = walk(
        at.dentcache.clone().ok_or(Errno::ENOTDIR)?,
        path,
        oflags & oflags::NOFOLLOW == 0,
    )?;

    // Open the target VNode.
    let vnode = match &cache.type_ {
        DentCacheType::Negative => o_creat_helper(cache.clone(), oflags & oflags::EXCLUSIVE != 0)?,
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
        NodeType::Fifo => todo!("FIFO file ops"),
        NodeType::CharDev => todo!("Character devide file ops"),
        NodeType::BlockDev => todo!("Block device file ops"),
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
pub fn link(old_path: &dyn PathSpec, new_path: &dyn PathSpec) -> EResult<()> {
    todo!();
}

/// Remove a file or directory.
/// Uses POSIX `rmdir` semantics iff `is_rmdir`, otherwise POSIX unlink semantics.
pub fn unlink(path: &dyn PathSpec, is_rmdir: bool) -> EResult<()> {
    todo!();
}

/// Create a new file or directory.
fn make_file(path: &dyn PathSpec, spec: NewFileSpec) -> EResult<Arc<dyn File>> {
    todo!();
}

/// Rename a file within the same filesystem.
pub fn rename(old_path: &dyn PathSpec, new_path: &dyn PathSpec) -> EResult<()> {
    todo!();
}

#[unsafe(no_mangle)]
unsafe extern "C" fn rust_fs_test() {
    fs_test();
}

pub fn fs_test() {
    // Hardcoded mount of a RamFS.
    let ramfs = RamFS::new().unwrap();
    let root_ops = ramfs.open_root().unwrap();

    let vfs = Arc::try_new(Vfs {
        ops: Mutex::new(Box::new(ramfs)),
        media: None,
        vnodes: Mutex::new(BTreeMap::new()),
        root: UnsafeCell::new(None),
        mountpoint: None,
        flags: AtomicU32::new(0),
    })
    .unwrap();

    let dentcache = DentCache {
        type_: DentCacheType::Directory(Mutex::new(DentCacheDir::EMPTY)),
        vfs: vfs.clone(),
        parent: None,
        vnode: Mutex::new(None),
        dirent: Dirent {
            ino: 1,
            type_: NodeType::Directory,
            name: b"/".into(),
            dirent_off: 0,
        },
    };

    let root = Arc::new(VNode {
        ops: Mutex::new(root_ops),
        inode: 1,
        vfs: vfs.clone(),
        dentcache: Some(Arc::new(dentcache)),
        flags: AtomicU32::new(0),
        type_: NodeType::Directory,
    });
    unsafe { *vfs.root.as_mut_unchecked() = Some(root) };

    *ROOT_FS.lock() = Some(vfs);

    // Try some simple operations on the RamFS.
    let file = open(&"/a.txt", oflags::CREATE | oflags::READ_WRITE).unwrap();
    file.write(b"This data").unwrap();
    file.seek(SeekMode::Set, 0).unwrap();
    let mut buf = [0u8; 9];
    let rlen = file.read(&mut buf).unwrap();
    logkf!(LogLevel::Debug, "Read data: {}: {:?}", rlen, &buf);
}
