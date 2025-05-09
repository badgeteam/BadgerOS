
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
// Do not block on sockets, FIFOs, TTYs, etc.
#define OFLAGS_NONBLOCK  0x00000100

// Bitmask of filesystem open flags that only the kernel may apply.
#define KOFLAGS_MASK    0xffff0000
// Kernel-only flag: Open for execution.
#define KOFLAGS_EXECUTE 0x00010000

// Bitmask of all flags valid for use with the `fs_pipe` function.
#define VALID_OFLAGS_PIPE OFLAGS_NONBLOCK
// Bitmask of all opening flags valid without the use of `OFLAGS_DIRECTORY` and without the use of `OFLAGS_EXCLUSIVE`.
#define VALID_OFLAGS_FILE                                                                                              \
    (OFLAGS_READWRITE | OFLAGS_APPEND | OFLAGS_TRUNCATE | OFLAGS_CREATE | OFLAGS_EXCLUSIVE | OFLAGS_CLOEXEC |          \
     OFLAGS_NONBLOCK | KOFLAGS_EXECUTE)
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
typedef long    file_t;
// Type used for file offsets.
typedef int64_t fileoff_t;
// Type used for file modes.
typedef int     mode_t;

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
    FILETYPE_FILE = 8,
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

// List of dirents as created by `fs_dir_read`.
typedef struct {
    // Pointer to packed array of `dirent_t`, each aligned to 8 bytes.
    // Dirent names are null-terminated.
    // Dirents are shortened at the name to conserve memory.
    void  *mem;
    // Byte size of `mem`.
    size_t size;
    // Number of dirents in the list.
    size_t ent_count;
} dirent_list_t;

// File or directory status.
typedef struct stat {
    // ID of device containing file.
    uint32_t  device;
    // Inode number.
    inode_t   inode;
    // File type and protection.
    mode_t    mode;
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

// Return value of `fs_pipe`.
typedef struct {
    file_t reader;
    file_t writer;
} fs_pipe_t;

// Try to mount a filesystem.
// Some filesystems (like RAMFS) do not use a block device, for which `media` must be NULL.
// Filesystems which do use a block device can often be automatically detected.
void fs_mount(
    badge_err_t *ec, char const *type, blkdev_t *media, file_t at, char const *path, size_t path_len, mountflags_t flags
);
// Try to unmount a filesystem.
// May fail if there any any files open on the target filesystem.
void        fs_umount(badge_err_t *ec, file_t at, char const *path, size_t path_len);
// Try to identify the filesystem stored in the block device
// Returns `NULL` on error or if the filesystem is unknown.
char const *fs_detect(badge_err_t *ec, blkdev_t *media);

// Test whether a path is a canonical path, but not for the existence of the file or directory.
// A canonical path starts with '/' and contains none of the following regex: `\.\.?/|//+`
bool fs_is_canonical_path(char const *path, size_t path_len);

// Get file status given file handler or path, optionally following the final symlink.
// If both `fd` and `path` are specified, `fd` is a directory handle to which `path` is relative.
// Otherwise, either `fd` or `path` is used to get the stat info.
// If `follow_link` is false, the last symlink in the path is not followed.
void fs_stat(badge_err_t *ec, file_t fd, char const *path, size_t path_len, bool follow_link, stat_t *stat_out);

// Create a new directory relative to a dir handle.
// If `at` is `FILE_NONE`, it is relative to the root dir.
void   fs_mkdir(badge_err_t *ec, file_t at, char const *path, size_t path_len);
// Open a directory for reading relative to a dir handle.
// If `at` is `FILE_NONE`, it is relative to the root dir.
file_t fs_dir_open(badge_err_t *ec, file_t at, char const *path, size_t path_len, oflags_t oflags);
// Remove a directory, which must be empty, relative to a dir handle.
// If `at` is `FILE_NONE`, it is relative to the root dir.
void   fs_rmdir(badge_err_t *ec, file_t at, char const *path, size_t path_len);

// Close a directory opened by `fs_dir_open`.
// Only raises an error if `dir` is an invalid directory descriptor.
void          fs_dir_close(badge_err_t *ec, file_t dir);
// Read all entries from a directory.
dirent_list_t fs_dir_read(badge_err_t *ec, file_t dir);

// Unlink a file from the given directory relative to a dir handle.
// If `at` is `FILE_NONE`, it is relative to the root dir.
// If this is the last reference to an inode, the inode is deleted.
// Fails if this is a directory.
void fs_unlink(badge_err_t *ec, file_t at, char const *path, size_t path_len);
// Create a new hard link from one path to another relative to their respective dirs.
// If `*_at` is `FILE_NONE`, it is relative to the root dir.
// Fails if `old_path` names a directory.
void fs_link(
    badge_err_t *ec,
    file_t       old_at,
    char const  *old_path,
    size_t       old_path_len,
    file_t       new_at,
    char const  *new_path,
    size_t       new_path_len
);
// Create a new symbolic link from one path to another, the latter relative to a dir handle.
// The `old_path` specifies a path that is relative to the symlink's location.
// If `new_at` is `FILE_NONE`, it is relative to the root dir.
void fs_symlink(
    badge_err_t *ec,
    char const  *target_path,
    size_t       target_path_len,
    file_t       link_at,
    char const  *link_path,
    size_t       link_path_len
);
// Create a new named FIFO at a path relative to a dir handle.
// If `at` is `FILE_NONE`, it is relative to the root dir.
void fs_mkfifo(badge_err_t *ec, file_t at, char const *path, size_t path_len);

// Create a new pipe with one read and one write end.
fs_pipe_t fs_pipe(badge_err_t *ec, int flags);
// Open a file for reading and/or writing relative to a dir handle.
// If `at` is `FILE_NONE`, it is relative to the root dir.
file_t    fs_open(badge_err_t *ec, file_t at, char const *path, size_t path_len, oflags_t oflags);
// Close a file opened by `fs_open`.
// Only raises an error if `file` is an invalid file descriptor or an I/O error occurs.
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
void fs_flush(badge_err_t *ec, file_t file);
