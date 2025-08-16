// SPDX-FileCopyrightText: 2025 Julian Scheffers <julian@scheffers.net>
// SPDX-FileType: SOURCE
// SPDX-License-Identifier: MIT

use alloc::{boxed::Box, sync::Arc, vec::Vec};
use cluster::{ClusterAlloc, ClusterChain};
use spec::{Bpb, Dirent, Header16, Header32, LfnEnt, attr};

use crate::{
    badgelib::utf8::{StaticString, StringLike},
    bindings::{
        error::{EResult, Errno},
        mutex::Mutex,
    },
};

use super::NAME_MAX;
use super::{
    FSDRIVERS, MakeFileSpec, NodeType, Stat,
    media::Media,
    vfs::{VNode, VNodeOps, Vfs, VfsDriver, VfsOps, mflags::MFlags},
};
use core::{
    any::Any,
    num::{self, NonZeroU8, NonZeroU32},
    ops::Index,
};

mod cluster;
mod spec;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
/// Types of FAT filesystem.
enum FatType {
    Fat12,
    Fat16,
    Fat32,
}

/// Either a cluster chain or the region where the FAT12/FAT16 root directory is.
enum FatFileStorage {
    /// The root directory for FAT12/FAT16, which is stored in a fixed location.
    Root16(u64),
    /// A chain of clusters; also used for FAT32 root directories.
    Clusters(ClusterChain),
}

/// A FAT file node.
/// Despite FAT not being designed for it, unlinked files can still be accessed by their VNode.
/// Note: Directories allow writing, which is used internally, and the outer VFS prevents the user from writing to directories.
struct FatVNode {
    /// Where this file is stored in the media.
    storage: FatFileStorage,
    /// The size of the file.
    len: u32,
    /// Offset of the parent dirent, if any.
    dirent_off: u64,
}

/// Helper function that gets a reference to the FAT filesystem from a VNode.
fn get_fatfs(vfs: &Vfs) -> &FatFS {
    unsafe { &*(vfs.ops.data().as_ref() as *const dyn VfsOps as *const FatFS) }
}

impl FatVNode {
    /// Get the on-disk offset of some byte of the file.
    /// Primarily used for dirents.
    fn disk_offset_of(&self, arc_self: &Arc<VNode>, offset: u32) -> u64 {
        let fatfs = get_fatfs(&arc_self.vfs);
        match &self.storage {
            FatFileStorage::Root16(x) => x + offset as u64,
            FatFileStorage::Clusters(chain) => {
                let cluster = chain.get(offset >> fatfs.cluster_size_exp).unwrap();
                ((cluster as u64) << fatfs.cluster_size_exp)
                    + (offset & !((1u32 << fatfs.cluster_size_exp) - 1)) as u64
            }
        }
    }

    /// Helper function to iterate dirents.
    /// Stops iteration if `dirent_func` return `Err(_)` or `Ok(false)`.
    fn iter_dirents(
        &self,
        arc_self: &Arc<VNode>,
        mut dirent_func: impl FnMut(u64, &Dirent, &str, Option<&str>) -> EResult<bool>,
    ) -> EResult<()> {
        let fatfs = get_fatfs(&arc_self.vfs);
        let mut lfn_buf = StaticString::<NAME_MAX>::new();
        let mut lfn_seq = Option::<u8>::None;
        let mut use_lfn = fatfs.allow_lfn;

        // Iterate over raw directory entries, only calling `dirent_func` once for each valid dirent.
        // If the LFN name ends up being too long, then the 8.3 format name is used instead.
        let mut offset = 0u32;
        while offset < self.len {
            let mut raw_dirent = [0u8; 32];
            self.read(arc_self, offset as u64, &mut raw_dirent)?;
            let raw_dirent = Dirent::from(raw_dirent);
            if raw_dirent.name[0] == 0 {
                // No more allocated dirents.
                break;
            } else if raw_dirent.name[0] == 0xe5 {
                // Free dirent.
            } else if raw_dirent.attr & attr::VOLUME_ID == 0 {
                let mut dirent = raw_dirent;
                dirent.from_le();

                // TODO: Try to read preceding LFN dirents.

                // Convert short filename.
                let mut sfn = StaticString::<12>::new();
                fatfs.short_name_to_str(&dirent.name, &mut sfn);

                // Check validity of dirent.
                if !dirent.name.iter().all(|&x| {
                    fatfs.is_valid_short_char(unsafe { char::from_u32_unchecked(x as u32) })
                }) {
                    arc_self.vfs.check_eio_failed();
                } else if !(dirent_func(
                    self.disk_offset_of(arc_self, offset),
                    &dirent,
                    sfn.as_ref(),
                    use_lfn.then_some(lfn_buf.as_ref()),
                )?) {
                    // Callback said to stop iterating.
                    break;
                }
            }

            offset += 32;
        }

        Ok(())
    }

    /// Converts a [`Dirent`] and a name to a [`super::Dirent`].
    fn convert_dirent(dirent_off: u64, dirent: &Dirent, name: &[u8]) -> EResult<super::Dirent> {
        let mut name_copy = Vec::try_with_capacity(name.len())?;
        name_copy.resize(name.len(), 0);
        name_copy.copy_from_slice(name);

        // Make up an inode number, which is required, but FAT doesn't have.
        let first_cluster =
            ((dirent.first_cluster_hi as u32) << 16) | dirent.first_cluster_lo as u32;
        let ino = if first_cluster == 0 {
            // If it has no data yet, derive inode from dirent offset.
            (dirent_off as u64) << 1
        } else {
            // If it has data allocated already, derive inode from first cluster.
            ((first_cluster << 1) + 1) as u64
        };

        let type_ = if dirent.attr & attr::DIRECTORY != 0 {
            NodeType::Directory
        } else {
            NodeType::Regular
        };

        Ok(super::Dirent {
            ino,
            type_,
            name: name_copy,
            dirent_off,
        })
    }
}

impl VNodeOps for FatVNode {
    fn write(&self, arc_self: &Arc<VNode>, offset: u64, wdata: &[u8]) -> EResult<()> {
        let fatfs = get_fatfs(&arc_self.vfs);
        if offset.checked_add(wdata.len() as u64).ok_or(Errno::EIO)? > self.len as u64 {
            return Err(Errno::EIO);
        }
        match &self.storage {
            FatFileStorage::Root16(x) => fatfs.media.write(x + offset, wdata),
            FatFileStorage::Clusters(chain) => chain.write(&fatfs, offset, wdata),
        }
    }

    fn read(&self, arc_self: &Arc<VNode>, offset: u64, rdata: &mut [u8]) -> EResult<()> {
        let fatfs = get_fatfs(&arc_self.vfs);
        if offset.checked_add(rdata.len() as u64).ok_or(Errno::EIO)? > self.len as u64 {
            return Err(Errno::EIO);
        }
        match &self.storage {
            FatFileStorage::Root16(x) => fatfs.media.read(x + offset, rdata),
            FatFileStorage::Clusters(chain) => chain.read(&fatfs, offset, rdata),
        }
    }

    fn resize(&mut self, arc_self: &Arc<VNode>, new_size: u64) -> EResult<()> {
        let new_size = TryInto::<u32>::try_into(new_size).map_err(|_| Errno::ENOSPC)?;
        let fatfs = get_fatfs(&arc_self.vfs);
        let new_clusters =
            ((new_size + (1 << fatfs.cluster_size_exp) - 1) >> fatfs.cluster_size_exp) as u32;
        match &mut self.storage {
            FatFileStorage::Root16(_) => {
                // For FAT12/FAT16 root directory, resizing is not supported.
                return Err(Errno::ENOSPC);
            }
            FatFileStorage::Clusters(chain) => {
                if new_clusters > chain.len() {
                    // Allocate additional clusters.
                    let extra_chain = fatfs
                        .cluster_alloc
                        .alloc_chain(new_clusters - chain.len())?;
                    if let Err(x) = chain.try_reserve(extra_chain.entries_len()) {
                        fatfs.cluster_alloc.free_chain(extra_chain);
                        return Err(x.into());
                    }

                    // Update the FAT entries.
                    // Note: If `FatFS::fat_set` fails, it will return EIO, causing the VFS to make this FS read-only.
                    let mut last_cluster = chain.last();
                    for cluster in &extra_chain {
                        fatfs
                            .fat_set(last_cluster, FatValue::Next(cluster))
                            .map_err(|_| Errno::EIO)?;
                        last_cluster = cluster;
                    }
                    fatfs
                        .fat_set(last_cluster, FatValue::Eoc)
                        .map_err(|_| Errno::EIO)?;

                    chain.extend(extra_chain);
                } else if new_clusters < chain.len() {
                    // Free excess clusters.
                    // Note: If `FatFS::fat_set` fails, it will return EIO, causing the VFS to make this FS read-only.
                    for cluster in chain.range(new_clusters, chain.len() - new_clusters) {
                        fatfs.cluster_alloc.free(cluster);
                        fatfs
                            .fat_set(cluster, FatValue::Free)
                            .map_err(|_| Errno::EIO)?;
                    }
                    chain.shorten(chain.len() - new_clusters);
                }
            }
        }
        self.len = new_size;
        Ok(())
    }

    fn find_dirent(&self, arc_self: &Arc<VNode>, name: &[u8]) -> EResult<super::Dirent> {
        let name = FatFS::trim_name_bytes(name);
        let mut res = Err(Errno::ENOENT);
        let res_ptr = &mut res;
        self.iter_dirents(arc_self, |off, dent, sfn, lfn| {
            if let Some(lfn) = lfn
                && FatFS::name_equals(lfn.as_bytes(), name)
            {
                *res_ptr = Ok(Self::convert_dirent(off, dent, lfn.as_bytes())?);
                Ok(false)
            } else if FatFS::name_equals(sfn.as_bytes(), name) {
                *res_ptr = Ok(Self::convert_dirent(off, dent, sfn.as_bytes())?);
                Ok(false)
            } else {
                Ok(true)
            }
        })?;
        res
    }

    fn get_dirents(&self, arc_self: &Arc<VNode>) -> EResult<Vec<super::Dirent>> {
        let mut out = Vec::new();
        self.iter_dirents(arc_self, |off, dent, sfn, lfn| {
            out.try_reserve(1)?;
            out.push(Self::convert_dirent(
                off,
                dent,
                lfn.unwrap_or(sfn).as_bytes(),
            )?);
            Ok(true)
        })?;
        Ok(out)
    }

    fn unlink(&mut self, arc_self: &Arc<VNode>, name: &[u8], is_rmdir: bool) -> EResult<()> {
        todo!()
    }

    fn link(&mut self, arc_self: &Arc<VNode>, name: &[u8], inode: &VNode) -> EResult<()> {
        todo!()
    }

    fn make_file(
        &mut self,
        arc_self: &Arc<VNode>,
        name: &[u8],
        spec: MakeFileSpec,
    ) -> EResult<Box<dyn VNodeOps>> {
        todo!()
    }

    fn rename(&mut self, arc_self: &Arc<VNode>, old_name: &[u8], new_name: &[u8]) -> EResult<()> {
        todo!()
    }

    fn readlink(&self, arc_self: &Arc<VNode>) -> EResult<Box<[u8]>> {
        todo!()
    }

    fn stat(&self, arc_self: &Arc<VNode>) -> EResult<Stat> {
        todo!()
    }

    fn get_inode(&self) -> u64 {
        match &self.storage {
            FatFileStorage::Root16(_) => 1,
            FatFileStorage::Clusters(chain) => (chain.get(0).unwrap() << 1) as u64 + 1u64,
        }
    }

    fn get_size(&self, arc_self: &Arc<VNode>) -> u64 {
        todo!()
    }

    fn get_type(&self, arc_self: &Arc<VNode>) -> NodeType {
        todo!()
    }

    fn sync(&self, arc_self: &Arc<VNode>) -> EResult<()> {
        let fatfs = get_fatfs(&arc_self.vfs);
        match &self.storage {
            FatFileStorage::Root16(x) => fatfs.media.sync(*x, self.len as u64),
            FatFileStorage::Clusters(chain) => chain.sync(fatfs),
        }
    }
}

/// FAT entry values.
enum FatValue {
    /// The next cluster in the chain.
    Next(u32),
    /// The end of the chain.
    Eoc,
    /// Free cluster.
    Free,
    /// Bad cluster.
    Bad,
}

/// A mounted FAT filesystem.
struct FatFS {
    /// The media this filesystem is mounted on.
    media: Media,
    /// The type of FAT filesystem (FAT12, FAT16, or FAT32).
    fat_type: FatType,
    /// Whether the long filename extension is enabled.
    allow_lfn: bool,
    /// Log-base 2 of the cluster size in bytes.
    cluster_size_exp: u32,
    /// Log-base 2 of the sector size in bytes.
    sector_size_exp: u32,
    /// The number of sectors per FAT.
    sectors_per_fat: u32,
    /// The number of clusters in the filesystem.
    cluster_count: u32,
    /// The cluster allocator for this filesystem.
    cluster_alloc: ClusterAlloc,
    /// Byte offset of the first data cluster.
    data_offset: u64,
    /// Sector offset of the first FAT.
    fat_sector: u32,
    /// Number of FATs.
    fat_count: u8,
    /// Active FAT index.
    active_fat: u8,
    /// Whether to mirror the FATs.
    mirror_fats: bool,
    /// Start cluster of the root directory, if FAT32.
    root_dir_cluster: u32,
    /// Offset of the root directory, if FAT12/FAT16.
    legacy_root_sector: u32,
    /// Size of the root directory in entries, if FAT12/FAT16.
    legacy_root_size: u32,
    /// Mutex used to protect FAT12 read-modify-write.
    fat12_mutex: Mutex<()>,
}

impl FatFS {
    /// Whether a character is valid in a short name entry.
    /// Rejects lower-case characters; they are stored upper-case on disk.
    /// TODO: Subject to get support for non-ASCII encodings, which would allow more values.
    pub fn is_valid_short_char(&self, value: char) -> bool {
        if value as u32 >= 0x7f {
            return false;
        }
        match value as u8 {
            b'"' => false,
            b'*' => false,
            b'+' => false,
            b',' => false,
            b'.' => false,
            b'/' => false,
            b':' => false,
            b';' => false,
            b'<' => false,
            b'=' => false,
            b'>' => false,
            b'?' => false,
            b'[' => false,
            b'\\' => false,
            b']' => false,
            b'|' => false,
            x => x >= 0x20,
        }
    }

    /// Whether a character is valid in a long name entry.
    pub fn is_valid_long_char(value: char) -> bool {
        if value as u32 > 0xff {
            return true;
        }
        match value as u8 {
            b'"' => false,
            b'*' => false,
            b'+' => false,
            b',' => false,
            b'.' => false,
            b'/' => false,
            b':' => false,
            b';' => false,
            b'<' => false,
            b'=' => false,
            b'>' => false,
            b'?' => false,
            b'[' => false,
            b'\\' => false,
            b']' => false,
            b'|' => false,
            0x7f => false,
            x => x >= 0x20,
        }
    }

    /// Convert long char to equivalent short char.
    /// Returns [`None`] if it should be implicitly removed.
    pub fn long_to_short_char(&self, long: u16) -> Option<NonZeroU8> {
        if long == ' ' as u16 {
            None
        } else if self.is_valid_short_char(unsafe { char::from_u32_unchecked(long as u32) }) {
            Some(unsafe { NonZeroU8::new_unchecked(long as u8) })
        } else {
            Some(unsafe { NonZeroU8::new_unchecked(b'_') })
        }
    }

    /// Determines whether a short name is suffixed by ~number.
    pub fn is_number_suffixed(short_name: &[u8; 11]) -> Option<u32> {
        // Ignore the extension because it's not where the ~number suffix goes.
        let short_name = &short_name[..8];
        // Remove trailing spaces.
        let short_name = &short_name[..short_name
            .iter()
            .rposition(|x| *x != b' ')
            .unwrap_or(short_name.len() - 1)
            + 1];

        // Find last tilde.
        let tilde = short_name.iter().rposition(|x| *x == b'~')?;
        let mut number = 0u32;

        // Parse the number after the tilde.
        for &x in &short_name[tilde + 1..] {
            if x < b'0' || x > b'9' {
                return None; // Invalid character after tilde.
            }
            number = number * 10 + (x - b'0') as u32;
        }

        Some(number)
    }

    /// Convert a long name into a short one.
    /// If any information is lost, the output name will be suffixed with `~1`.
    pub fn long_to_short_name(&self, long_name: &[u16], short_out: &mut [u8; 11]) {
        short_out.fill(b' ');
        let mut info_lost = false;

        // Set extension.
        let ext = long_name.iter().rposition(|x| *x == '.' as u16);
        if let Some(ext) = ext {
            let mut i = 0;
            for char in &long_name[ext + 1..] {
                if let Some(char) = self.long_to_short_char(*char) {
                    if i < 3 {
                        short_out[8 + i] = char.into();
                        i += 1;
                    } else {
                        info_lost = true;
                    }
                } else {
                    info_lost = true;
                }
            }
        }

        // Set name.
        let mut i = 0;
        for char in &long_name[..ext.unwrap_or(long_name.len())] {
            if let Some(char) = self.long_to_short_char(*char) {
                if i < 8 {
                    short_out[i] = char.into();
                    i += 1;
                } else {
                    info_lost = true;
                }
            } else {
                info_lost = true;
            }
        }

        // Suffix the output name with ~1 if info is lost...
        if info_lost {
            // ...and it is not already suffixed with ~number.
            if Self::is_number_suffixed(short_out).is_some() {
                return;
            }
        }
    }

    /// Convert a short name into a string.
    /// Returns whether `name_out` had enough capacity to store it.
    pub fn short_name_to_str(&self, short: &[u8; 11], name_out: &mut impl StringLike) -> bool {
        // Find portion of name that is not just spaces.
        let name_end = short[..8]
            .iter()
            .rposition(|&x| x != b' ')
            .map_or(0, |x| x + 1);
        let ext_end = short[8..11]
            .iter()
            .rposition(|&x| x != b' ')
            .map_or(0, |x| x + 1);

        for &x in &short[..name_end] {
            if !name_out.push(unsafe { char::from_u32_unchecked(x as u32) }) {
                return false;
            }
        }

        if ext_end > 0 {
            if !name_out.push('.') {
                return false;
            }
            for &x in &short[8..8 + ext_end] {
                if !name_out.push(unsafe { char::from_u32_unchecked(x as u32) }) {
                    return false;
                }
            }
        }

        true
    }

    /// Trim beginning and end off of names according to FAT rules.
    /// Operates on a UTF-8 string slice.
    fn trim_name<'a>(name: &'a str) -> &'a str {
        name.trim_ascii_start()
            .trim_end_matches(|x| x == ' ' || x == '.')
    }

    /// Trim beginning and end off of names according to FAT rules.
    /// Operates on bytes, some of which may be ASCII, instead of UTF-8.
    fn trim_name_bytes<'a>(mut name: &'a [u8]) -> &'a [u8] {
        while let Some(x) = name.first()
            && *x == b' '
        {
            name = &name[1..];
        }
        while let Some(x) = name.last()
            && (*x == b' ' || *x == b'.')
        {
            name = &name[..name.len() - 1];
        }
        name
    }

    /// Compares two names for equality by FAT rules.
    /// Assumes that the names are already trimmed.
    fn name_equals(a: &[u8], b: &[u8]) -> bool {
        if a.len() != b.len() {
            return false;
        }
        for i in 0..a.len() {
            if !a[i].eq_ignore_ascii_case(&b[i]) {
                return false;
            }
        }
        true
    }

    /// Write the FAT next pointer for a cluster.
    fn fat_set_impl(self: &FatFS, fat_offset: u64, cluster: u32, value: u32) -> EResult<()> {
        match self.fat_type {
            FatType::Fat12 => {
                let mut bytes = [0u8; 2];
                self.media
                    .read(fat_offset + (cluster as u64 * 3 / 2), &mut bytes)?;

                if cluster & 1 == 0 {
                    bytes[0] = value as u8;
                    bytes[1] = (bytes[1] & 0xf0) | ((value >> 8) as u8 & 0x0f);
                } else {
                    bytes[0] = (bytes[0] & 0x0f) | (value << 4) as u8;
                    bytes[1] = (value >> 4) as u8;
                }

                self.media
                    .write(fat_offset + (cluster as u64 * 3 / 2), &bytes)?;
            }
            FatType::Fat16 => {
                let bytes = (value as u16).to_le_bytes();
                self.media
                    .write(fat_offset + (cluster as u64 * 2), &bytes)?;
            }
            FatType::Fat32 => {
                // FAT requires preserving the upper bits, but nothing actually uses them so IDC.
                let bytes = value.to_le_bytes();
                self.media
                    .write(fat_offset + (cluster as u64 * 4), &bytes)?;
            }
        }
        Ok(())
    }

    /// Write the FAT next pointer for a cluster.
    fn fat_set(&self, cluster: u32, value: FatValue) -> EResult<()> {
        debug_assert!(cluster < self.cluster_count);
        let cluster = cluster + 2;
        let value = match value {
            FatValue::Next(x) => {
                if x >= self.cluster_count {
                    return Err(Errno::EIO);
                }
                x + 2
            }
            FatValue::Eoc => 0x0fff_ffff,
            FatValue::Free => 0x0000_0000,
            FatValue::Bad => 0x0fff_fff7,
        };
        if self.mirror_fats {
            for i in 0..self.fat_count {
                self.fat_set_impl(
                    (self.fat_sector as u64 + i as u64 * self.sectors_per_fat as u64)
                        << self.sector_size_exp,
                    cluster,
                    value,
                )?;
            }
            Ok(())
        } else {
            self.fat_set_impl(
                (self.fat_sector as u64) << self.sector_size_exp,
                cluster,
                value,
            )
        }
    }

    /// Read the FAT next pointer for a cluster.
    fn fat_get(&self, cluster: u32) -> EResult<FatValue> {
        debug_assert!(cluster < self.cluster_count);
        let cluster = cluster + 2;
        let fat_offset = (self.fat_sector as u64
            + self.active_fat as u64 * self.sectors_per_fat as u64)
            << self.sector_size_exp;
        let value = match self.fat_type {
            FatType::Fat12 => {
                // Read FAT12 entry.
                let _guard = self.fat12_mutex.lock_shared();
                let mut bytes = [0u8; 2];
                self.media
                    .read(fat_offset + (cluster as u64 * 3 / 2), &mut bytes)?;
                let tmp = if cluster & 1 == 0 {
                    bytes[0] as u16 | ((bytes[1] as u16 & 0x0f) << 8)
                } else {
                    (bytes[0] >> 4) as u16 | ((bytes[1] as u16) << 4)
                };
                if tmp >= 0xff7 {
                    tmp as u32 + 0xffff_f000
                } else {
                    tmp as u32
                }
            }
            FatType::Fat16 => {
                // Read FAT16 entry.
                let mut bytes = [0u8; 2];
                self.media
                    .read(fat_offset + (cluster as u64 * 2), &mut bytes)?;
                let tmp = u16::from_le_bytes(bytes);
                if tmp >= 0xfff7 {
                    tmp as u32 + 0xffff_0000
                } else {
                    tmp as u32
                }
            }
            FatType::Fat32 => {
                // Read FAT32 entry.
                let mut bytes = [0u8; 4];
                self.media
                    .read(fat_offset + (cluster as u64 * 4), &mut bytes)?;
                u32::from_le_bytes(bytes) & 0x0fff_ffff
            }
        };
        match value {
            0 => Ok(FatValue::Free),
            0x0fff_fff7 => Ok(FatValue::Bad),
            0x0fff_ffff => Ok(FatValue::Eoc),
            x if x >= 2 && x < self.cluster_count + 2 => Ok(FatValue::Next(x - 2)),
            _ => Err(Errno::EIO),
        }
    }

    /// Try to read a cluster chain.
    fn read_chain(&self, self_arc: &Arc<Vfs>, start_cluster: u32) -> EResult<ClusterChain> {
        let mut chain = ClusterChain::new();

        let mut cluster = start_cluster;
        loop {
            chain.try_reserve(1)?;
            chain.push(cluster);
            match self.fat_get(cluster)? {
                FatValue::Next(x) => cluster = x,
                FatValue::Eoc => break,
                _ => {
                    self_arc.check_eio_failed();
                    break;
                }
            }
        }

        Ok(chain)
    }
}

impl VfsOps for FatFS {
    fn open_root(&self, self_arc: &Arc<Vfs>) -> EResult<Box<dyn VNodeOps>> {
        if self.fat_type == FatType::Fat32 {
            let chain = self.read_chain(self_arc, self.root_dir_cluster)?;
            let len = chain.len() << self.cluster_size_exp;
            Ok(Box::<dyn VNodeOps>::from(Box::try_new(FatVNode {
                storage: FatFileStorage::Clusters(chain),
                len,
                dirent_off: 0,
            })?))
        } else {
            Ok(Box::<dyn VNodeOps>::from(Box::try_new(FatVNode {
                storage: FatFileStorage::Root16(
                    (self.legacy_root_sector as u64) << self.sector_size_exp,
                ),
                len: self.legacy_root_size * 32,
                dirent_off: 0,
            })?))
        }
    }

    fn open(
        &self,
        self_arc: &Arc<Vfs>,
        cached_dirent: &super::Dirent,
    ) -> EResult<Box<dyn VNodeOps>> {
        // Read FAT dirent from disk.
        let mut dirent = [0u8; size_of::<Dirent>()];
        self.media.read(cached_dirent.dirent_off, &mut dirent)?;
        let mut dirent = Dirent::from(dirent);

        // Read the cluster chain.
        dirent.from_le();
        let start_cluster =
            ((dirent.first_cluster_hi as u32) << 16) | dirent.first_cluster_lo as u32;
        let chain = self.read_chain(self_arc, start_cluster.checked_sub(2).ok_or(Errno::EIO)?)?;

        Ok(Box::<dyn VNodeOps>::from(Box::try_new(FatVNode {
            storage: FatFileStorage::Clusters(chain),
            len: dirent.size,
            dirent_off: cached_dirent.dirent_off,
        })?))
    }

    fn rename(
        &self,
        self_arc: &Arc<Vfs>,
        src_dir: &VNode,
        src_name: &[u8],
        dest_dir: &VNode,
        dest_name: &[u8],
    ) -> EResult<()> {
        todo!()
    }
}

/// The FAT filesystem driver.
struct FatFSDriver {
    /// Whether the long filename extension is enabled.
    allow_lfn: bool,
}

impl VfsDriver for FatFSDriver {
    fn detect(&self, media: &Media) -> EResult<bool> {
        Ok(false)
    }

    fn mount(&self, media: Option<Media>, mflags: MFlags) -> EResult<Box<dyn VfsOps>> {
        // Read the BPB.
        let media = media.ok_or(Errno::ENODEV)?;
        let mut bpb = [0u8; size_of::<Bpb>()];
        media.read(0, &mut bpb)?;
        let mut bpb = Bpb::from(bpb);
        bpb.from_le();

        if bpb.bytes_per_sector < 512
            || bpb.bytes_per_sector > 4096
            || bpb.bytes_per_sector.count_ones() != 1
        {
            return Err(Errno::EIO);
        }
        if bpb.sectors_per_cluster == 0 {
            return Err(Errno::EIO);
        }

        let mut header16 = [0u8; size_of::<Header16>()];
        let mut header32 = [0u8; size_of::<Header32>()];

        // Read the header.
        let sector_count = if bpb.sector_count_32 != 0 {
            media.read(size_of::<Bpb>() as u64, &mut header32)?;
            media.read(
                size_of::<Bpb>() as u64 + size_of::<Header32>() as u64,
                &mut header32,
            )?;
            bpb.sector_count_32
        } else {
            media.read(size_of::<Bpb>() as u64, &mut header16)?;
            bpb.sector_count_16 as u32
        };
        let mut header16 = Header16::from(header16);
        let mut header32 = Header32::from(header32);
        header16.from_le();
        header32.from_le();

        // Determine various parameters.
        let sectors_per_fat = if bpb.sectors_per_fat_16 != 0 {
            bpb.sectors_per_fat_16 as u32
        } else {
            header32.sectors_per_fat_32
        };
        let legacy_root_size = (bpb.root_entry_count as u32 * 32 + bpb.bytes_per_sector as u32 - 1)
            / bpb.bytes_per_sector as u32;
        let data_sectors = sector_count
            - bpb.reserved_sector_count as u32
            - bpb.fat_count as u32 * sectors_per_fat
            - legacy_root_size;
        let cluster_count = data_sectors / bpb.sectors_per_cluster as u32;
        let cluster_size_exp = bpb.sectors_per_cluster.ilog2();
        let fat_sector = bpb.reserved_sector_count as u32;
        let legacy_root_sector = fat_sector + sectors_per_fat * bpb.fat_count as u32;
        let data_sector = legacy_root_sector + legacy_root_size;

        // Determine filesystem type.
        let fat_type = if cluster_count < 4085 {
            FatType::Fat12
        } else if cluster_count < 65525 {
            FatType::Fat16
        } else {
            FatType::Fat32
        };
        let root_dir_cluster = header32.first_root_cluster.wrapping_sub(2);

        Ok(Box::<dyn VfsOps>::from(Box::try_new(FatFS {
            media,
            fat_type,
            allow_lfn: self.allow_lfn,
            cluster_size_exp,
            cluster_count,
            cluster_alloc: ClusterAlloc::new(cluster_count)?,
            data_offset: data_sector as u64 * bpb.bytes_per_sector as u64,
            sectors_per_fat,
            active_fat: header32.extra_flags as u8 & 15,
            mirror_fats: header32.extra_flags & 0x80 == 0,
            fat_sector,
            fat_count: bpb.fat_count,
            legacy_root_sector,
            legacy_root_size,
            fat12_mutex: Mutex::new(()),
            sector_size_exp: bpb.bytes_per_sector.ilog2(),
            root_dir_cluster,
        })?))
    }
}

fn register_fatfs() {
    FSDRIVERS
        .lock()
        .insert("vfat".into(), Box::new(FatFSDriver { allow_lfn: true }));
    FSDRIVERS
        .lock()
        .insert("msdos".into(), Box::new(FatFSDriver { allow_lfn: false }));
}

register_kmodule!(ramfs, [1, 0, 0], register_fatfs);
