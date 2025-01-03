
// SPDX-License-Identifier: MIT

#pragma once

#include "attributes.h"
#include "badge_err.h"
#include "blockdevice.h"

// Maximum number of mountable filesystems.
#define FILESYSTEM_MOUNT_MAX   8
// Maximum supported filename length.
#define FILESYSTEM_NAME_MAX    255
// Maximum supported path length.
#define FILESYSTEM_PATH_MAX    511
// Maximum supported symlink nesting.
#define FILESYSTEM_SYMLINK_MAX 8
// Maximum supported directory entries.
#define FILESYSTEM_DIRENT_MAX  64

// Mount as read-only filesystem (default: read-write).
#define MOUNTFLAGS_READONLY 0x00000001
// Filesystem mounting flags.
typedef uint32_t mountflags_t;

// Open for read-only.
#define OFLAGS_READONLY  0x00000001
// Open for write-only.
#define OFLAGS_WRITEONLY 0x00000002
// Open for read and write.
#define OFLAGS_READWRITE 0x00000003
// Seek to the end on opening.
#define OFLAGS_APPEND    0x00000004
// Truncate on opening.
#define OFLAGS_TRUNCATE  0x00000008
// Create if it doesn't exist on opening.
#define OFLAGS_CREATE    0x00000010
// Error if it exists on opening.
#define OFLAGS_EXCLUSIVE 0x00000020
// Do not inherit to child process.
#define OFLAGS_CLOEXEC   0x00000040
// Open a directory instead of a file.
#define OFLAGS_DIRECTORY 0x00000080

// Bitmask of all opening flags valid without the use of `OFLAGS_DIRECTORY` and without the use of `OFLAGS_EXCLUSIVE`.
#define VALID_OFLAGS_FILE                                                                                              \
    (OFLAGS_READWRITE | OFLAGS_APPEND | OFLAGS_TRUNCATE | OFLAGS_CREATE | OFLAGS_EXCLUSIVE | OFLAGS_CLOEXEC)
// Bitmask of all opening flags valid in conjunction with `OFLAGS_DIRECTORY`.
#define VALID_OFLAGS_DIRECTORY (OFLAGS_DIRECTORY | OFLAGS_CREATE | OFLAGS_EXCLUSIVE | OFLAGS_READONLY | OFLAGS_CLOEXEC)

// File opening mode flags.
typedef uint32_t oflags_t;

// Value used for absent inodes.
#define INODE_NONE ((inode_t)0)
// Type used for inode numbers.
typedef long inode_t;

// Value used for absent file / directory handle.
#define FILE_NONE ((file_t) - 1)
// Type used for file / directory handles in the kernel.
typedef int  file_t;
// Type used for file offsets.
typedef long fileoff_t;

// Supported filesystem types.
typedef enum {
    // Unknown / auto-detect filesystem type.
    FS_TYPE_UNKNOWN,
    // FAT12, FAT16 or FAT32.
    FS_TYPE_FAT,
    // RAM filesystem.
    FS_TYPE_RAMFS,
} fs_type_t;

// Modes for VFS seek.
typedef enum {
    // Seek from start of file.
    SEEK_ABS = -1,
    // Seek from current position.
    SEEK_CUR = 0,
    // Seek from end of file.
    SEEK_END = 1,
} fs_seek_t;

// EXT2/3/4, unix file types.
typedef enum {
    // Unix socket
    FILETYPE_SOCK = 12,
    // Symbolic link
    FILETYPE_LINK = 10,
    // Regular file
    FILETYPE_REG  = 8,
    // Block device
    FILETYPE_BLK  = 6,
    // Directory
    FILETYPE_DIR  = 4,
    // Character device
    FILETYPE_CHR  = 2,
    // FIFO
    FILETYPE_FIFO = 1,
} filetype_t;

// Directory entry as read from a directory handle.
// The `record_len` field indicates the total size of the `dirent_t` and is therefor the offset to the next `dirent_t`.
// If `record_len < sizeof(dirent_t)`, `name` is smaller.
// This means that `record_len >= sizeof(dirent_t) - FILESYSTEM_NAME_MAX + 1`.
typedef struct {
    // Length of the file entry record.
    fileoff_t record_len;
    // Inode number; gauranteed to be unique per physical file per filesystem.
    inode_t   inode;
    // Node is a directory.
    bool      is_dir;
    // Node is a symbolic link.
    bool      is_symlink;
    // Length of the filename.
    fileoff_t name_len;
    // Filename.
    char      name[FILESYSTEM_NAME_MAX + 1];
} dirent_t;

// File or directory status.
typedef struct stat {
    // ID of device containing file.
    uint32_t  device;
    // Inode number.
    inode_t   inode;
    // File type and protection.
    uint16_t  mode;
    // Number of hard links.
    size_t    links;
    // Owner user ID.
    // TODO: User ID type?
    int       uid;
    // Owner group ID.
    // TODO: Group ID type?
    int       gid;
    // File size in bytes.
    fileoff_t size;
} stat_t;

// Try to mount a filesystem.
// Some filesystems (like RAMFS) do not use a block device, for which `media` must be NULL.
// Filesystems which do use a block device can often be automatically detected.
void      fs_mount(badge_err_t *ec, fs_type_t type, blkdev_t *media, char const *mountpoint, mountflags_t flags);
// Unmount a filesystem.
// Only raises an error if there isn't a valid filesystem to unmount.
void      fs_umount(badge_err_t *ec, char const *mountpoint);
// Try to identify the filesystem stored in the block device
// Returns `FS_TYPE_UNKNOWN` on error or if the filesystem is unknown.
fs_type_t fs_detect(badge_err_t *ec, blkdev_t *media);

// Test whether a path is a canonical path, but not for the existence of the file or directory.
// A canonical path starts with '/' and contains none of the following regex: `\.\.?/|//+`
bool fs_is_canonical_path(char const *path);
// Get file status given path.
bool fs_stat(badge_err_t *ec, stat_t *stat_out, char const *path);
// Get file status given path.
// If the path ends in a symlink, show it's status instead of that of the target file.
bool fs_lstat(badge_err_t *ec, stat_t *stat_out, char const *path);
// Get file status given file handle.
bool fs_fstat(badge_err_t *ec, stat_t *stat_out, file_t file);

// Create a new directory.
// Returns whether the target exists and is a directory.
bool   fs_dir_create(badge_err_t *ec, char const *path);
// Open a directory for reading.
file_t fs_dir_open(badge_err_t *ec, char const *path, oflags_t oflags);
// Close a directory opened by `fs_dir_open`.
// Only raises an error if `dir` is an invalid directory descriptor.
void   fs_dir_close(badge_err_t *ec, file_t dir);
// Read the current directory entry.
// Returns whether a directory entry was successfully read.
bool   fs_dir_read(badge_err_t *ec, dirent_t *dirent_out, file_t dir);

// Open a file for reading and/or writing.
file_t    fs_open(badge_err_t *ec, char const *path, oflags_t oflags);
// Close a file opened by `fs_open`.
// Only raises an error if `file` is an invalid file descriptor.
void      fs_close(badge_err_t *ec, file_t file);
// Read bytes from a file.
// Returns the amount of data successfully read.
fileoff_t fs_read(badge_err_t *ec, file_t file, void *readbuf, fileoff_t readlen);
// Write bytes to a file.
// Returns the amount of data successfully written.
fileoff_t fs_write(badge_err_t *ec, file_t file, void const *writebuf, fileoff_t writelen);
// Get the current offset in the file.
fileoff_t fs_tell(badge_err_t *ec, file_t file);
// Set the current offset in the file.
// Returns the new offset in the file.
fileoff_t fs_seek(badge_err_t *ec, file_t file, fileoff_t off, fs_seek_t seekmode);
// Force any write caches to be flushed for a given file.
// If the file is `FILE_NONE`, all open files are flushed.
void      fs_flush(badge_err_t *ec, file_t file);
