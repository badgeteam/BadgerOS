use core::{
    ffi::{CStr, c_void},
    mem::MaybeUninit,
};

use alloc::{string::String, vec::Vec};

use crate::bindings::{
    device::class::block::BlockDevice,
    error::{EResult, Errno},
    raw::{
        self, fileoff_t, fs_media__bindgen_ty_1, fs_media_t, fs_media_type_t_FS_MEDIA_BLKDEV,
        fs_media_type_t_FS_MEDIA_RAM, mountflags_t, stat_t,
    },
};

/// An opened file handle.
#[derive(Debug, Clone, Copy)]
pub struct File {
    inner: i32,
}
unsafe impl Send for File {}
unsafe impl Sync for File {}

/// A thing that can specify a file location in some way.
pub trait PathSpec {
    /// Convert this into a byte slice representing a path.
    fn as_path_spec<'a>(&'a self) -> &'a [u8];
    /// Convert this into a byte slice representing a path.
    fn as_raw_path_spec(&self) -> (*const u8, usize) {
        let spec = self.as_path_spec();
        (spec.as_ptr(), spec.len())
    }
}

impl PathSpec for String {
    fn as_path_spec<'a>(&'a self) -> &'a [u8] {
        self.as_bytes()
    }
}
impl PathSpec for &str {
    fn as_path_spec<'a>(&'a self) -> &'a [u8] {
        self.as_bytes()
    }
}
impl PathSpec for CStr {
    fn as_path_spec<'a>(&'a self) -> &'a [u8] {
        self.to_bytes()
    }
}

/// A thing that can specify a filesystem location possibly relative to an existing file.
pub trait FileLocSpec {
    /// Convert this into a tuple of file and a path.
    fn as_loc_spec<'a>(&'a self) -> (Option<File>, &'a [u8]);
    /// Convert this into a tuple of file and a path.
    fn as_raw_loc_spec(&self) -> (i32, *const u8, usize) {
        let loc = self.as_loc_spec();
        (
            loc.0.map(File::into_raw).unwrap_or(-1),
            loc.1.as_ptr(),
            loc.1.len(),
        )
    }
}

impl<T: PathSpec> FileLocSpec for T {
    fn as_loc_spec<'a>(&'a self) -> (Option<File>, &'a [u8]) {
        (None, self.as_path_spec())
    }
}
impl<T: PathSpec> FileLocSpec for (File, T) {
    fn as_loc_spec<'a>(&'a self) -> (Option<File>, &'a [u8]) {
        (Some(self.0), self.1.as_path_spec())
    }
}
impl<T: PathSpec> FileLocSpec for (T, File) {
    fn as_loc_spec<'a>(&'a self) -> (Option<File>, &'a [u8]) {
        (Some(self.1), self.0.as_path_spec())
    }
}

impl File {
    /// Create a file from a raw handle.
    pub const fn from_raw(raw: i32) -> Option<Self> {
        if raw >= 0 {
            Some(Self { inner: raw })
        } else {
            None
        }
    }
    /// Convert a file into a raw handle.
    pub const fn into_raw(self) -> i32 {
        self.inner
    }
    /// Open (and possibly create) a file.
    pub fn open(loc: impl FileLocSpec, flags: u32) -> EResult<Self> {
        let loc = loc.as_raw_loc_spec();
        Ok(Self {
            inner: Errno::check_u32(unsafe { raw::fs_open(loc.0, loc.1, loc.2, flags) })? as i32,
        })
    }
    /// Write bytes to the file.
    pub fn write(&self, buffer: &[u8]) -> EResult<fileoff_t> {
        Errno::check_u64(unsafe {
            raw::fs_write(
                self.inner,
                buffer.as_ptr() as *const c_void,
                buffer.len() as fileoff_t,
            )
        })
        .map(|x| x as fileoff_t)
    }
    /// Read file bytes into an existing array.
    pub fn read_into(&self, buffer: &mut [u8]) -> EResult<fileoff_t> {
        Errno::check_u64(unsafe {
            raw::fs_read(
                self.inner,
                buffer.as_mut_ptr() as *mut c_void,
                buffer.len() as fileoff_t,
            )
        })
        .map(|x| x as fileoff_t)
    }
    /// Read file bytes into a heap-allocated array.
    pub fn read(&self, max_len: usize) -> EResult<Vec<u8>> {
        let mut buf = Vec::try_with_capacity(max_len)?;
        buf.resize(max_len, 0);
        let len = Errno::check_u64(unsafe {
            raw::fs_read(
                self.inner,
                buf.as_mut_ptr() as *mut c_void,
                max_len as fileoff_t,
            )
        })?;
        buf.resize(len as usize, 0);
        Ok(buf)
    }
    /// Stat the underlying inode.
    pub fn stat(&self) -> EResult<stat_t> {
        let mut statbuf = MaybeUninit::<stat_t>::uninit();
        Errno::check(unsafe {
            raw::fs_stat(
                self.inner,
                0 as *const u8,
                0,
                false,
                &raw mut statbuf as *mut stat_t,
            )
        })?;
        Ok(unsafe { statbuf.assume_init() })
    }
}

/// Specifies some type of media a filesystem can be mounted on.
pub enum MediaType {
    Block(BlockDevice),
    Ram(Vec<u8>),
}

/// Specifies a partition to mount a filesystem on.
pub struct Media {
    /// Partition byte offset.
    pub offset: u64,
    /// Partition byte size.
    pub size: u64,
    /// Partition underlying storage.
    pub storage: MediaType,
}

impl Into<fs_media_t> for Media {
    fn into(self) -> fs_media_t {
        fs_media_t {
            type_: match self.storage {
                MediaType::Block(_) => fs_media_type_t_FS_MEDIA_BLKDEV,
                MediaType::Ram(_) => fs_media_type_t_FS_MEDIA_RAM,
            },
            part_offset: self.offset,
            part_length: self.size,
            __bindgen_anon_1: unsafe {
                match self.storage {
                    MediaType::Block(dev) => fs_media__bindgen_ty_1 {
                        blkdev: dev.leak().as_mut(),
                    },
                    MediaType::Ram(vec) => fs_media__bindgen_ty_1 {
                        ram: vec.into_raw_parts().0,
                    },
                }
            },
        }
    }
}

/// Mount a filesystem.
pub fn mount(
    type_: &CStr,
    media: Option<Media>,
    loc: impl FileLocSpec,
    flags: mountflags_t,
) -> EResult<()> {
    let loc = loc.as_raw_loc_spec();
    let mut media: Option<fs_media_t> = media.map(Into::into);
    Errno::check(unsafe {
        raw::fs_mount(
            type_.as_ptr(),
            media
                .as_mut()
                .map(|x| x as *mut fs_media_t)
                .unwrap_or(0 as *mut fs_media_t),
            loc.0,
            loc.1,
            loc.2,
            flags,
        )
    })
}

/// Unlink a file.
pub fn unlink(loc: impl FileLocSpec) -> EResult<()> {
    let loc = loc.as_raw_loc_spec();
    Errno::check(unsafe { raw::fs_unlink(loc.0, loc.1, loc.2) })
}
/// Create a directory.
pub fn mkdir(loc: impl FileLocSpec) -> EResult<()> {
    let loc = loc.as_raw_loc_spec();
    Errno::check(unsafe { raw::fs_mkdir(loc.0, loc.1, loc.2) })
}
/// Remove a directory.
pub fn rmdir(loc: impl FileLocSpec) -> EResult<()> {
    let loc = loc.as_raw_loc_spec();
    Errno::check(unsafe { raw::fs_rmdir(loc.0, loc.1, loc.2) })
}
/// Create a FIFO special file.
pub fn mkfifo(loc: impl FileLocSpec) -> EResult<()> {
    let loc = loc.as_raw_loc_spec();
    Errno::check(unsafe { raw::fs_mkfifo(loc.0, loc.1, loc.2) })
}
/// Create a new symbolic link.
pub fn symlink(link_loc: impl FileLocSpec, target_path: impl PathSpec) -> EResult<()> {
    let link_loc = link_loc.as_raw_loc_spec();
    let target_path = target_path.as_raw_path_spec();
    Errno::check(unsafe {
        raw::fs_symlink(
            target_path.0,
            target_path.1,
            link_loc.0,
            link_loc.1,
            link_loc.2,
        )
    })
}
/// Create a new hard link.
pub fn link(orig_loc: impl FileLocSpec, new_loc: impl FileLocSpec) -> EResult<()> {
    let orig_loc = orig_loc.as_raw_loc_spec();
    let new_loc = new_loc.as_raw_loc_spec();
    Errno::check(unsafe {
        raw::fs_link(
            orig_loc.0, orig_loc.1, orig_loc.2, new_loc.0, new_loc.1, new_loc.2,
        )
    })
}
/// Get file status from a location spec.
pub fn stat(loc: impl FileLocSpec, follow_link: bool) -> EResult<stat_t> {
    let loc = loc.as_raw_loc_spec();
    let mut statbuf = MaybeUninit::<stat_t>::uninit();
    Errno::check(unsafe {
        raw::fs_stat(
            loc.0,
            loc.1,
            loc.2,
            follow_link,
            &raw mut statbuf as *mut stat_t,
        )
    })?;
    Ok(unsafe { statbuf.assume_init() })
}
