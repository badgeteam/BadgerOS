
// SPDX-License-Identifier: MIT

#pragma once

// VFS shared opened file handle.
// Shared between all file handles referring to the same file.
typedef struct vfs_file_obj   vfs_file_obj_t;
// VFS opened file handle.
typedef struct vfs_file_desc  vfs_file_desc_t;
// VFS mounted filesystem.
typedef struct vfs            vfs_t;
// Filesystem implementation info.
typedef struct fs_driver      fs_driver_t;
// VFS information used to manage FIFOs.
typedef struct vfs_fifo_obj   vfs_fifo_obj_t;
// Device special file vtable.
typedef struct devfile_vtable devfile_vtable_t;
// Device special file data.
typedef struct devfile        devfile_t;
// Dirent cache entry.
typedef struct dentcache      dentcache_t;

// Indicates that a function returns either a `vfs_file_obj_t *` or an -errno.
typedef struct {
    int             errno;
    vfs_file_obj_t *fobj;
} errno_fobj_t;

// Indicates that a function returns either a `vfs_file_desc_t *` or an -errno.
typedef struct {
    int              errno;
    vfs_file_desc_t *fd;
} errno_fd_t;

#include "filesystem.h"
#include "filesystem/media.h"
#include "filesystem/vfs_vtable.h"
#include "map.h"
#include "mutex.h"
#include "time.h"


// VFS shared opened file handle.
// Shared between all file handles referring to the same file.
struct vfs_file_obj {
    // Reference count (how many `vfs_file_desc_t` reference this).
    atomic_int       refcount;
    // Type of file this is.
    filetype_t       type;
    // Current file size.
    fileoff_t        size;
    // Filesystem-specific information.
    void            *cookie;
    // Inode number (gauranteed to be unique per VFS).
    // No file or directory may have the same inode number.
    // Any file or directory is required to name an inode number of 1 or higher.
    inode_t          inode;
    // Link count (how many names reference this inode).
    // When 0 and after the last file object is closed, the file is deleted.
    uint32_t         links;
    // Pointer to the VFS on which this file exists.
    vfs_t           *vfs;
    // Handle mutex for concurrency.
    mutex_t          mutex;
    // FS mounted in this directory, if any.
    vfs_t           *mounted_fs;
    // Handle references the root directory of a mounted filesystem.
    // Not to be confused with the mountpoint directory.
    bool             is_vfs_root;
    // Buffer for pipes and FIFOs.
    vfs_fifo_obj_t  *fifo;
    // Handle for device special files.
    devfile_t const *devfile;
    // Dirent cache entry for directories.
    dentcache_t     *dentcache;
};

// VFS opened file handle.
struct vfs_file_desc {
    // Reference count (how many threads reference this).
    atomic_int refcount;
    // Current access position.
    // Note: Must be bounds-checked on every file I/O.
    fileoff_t  offset;
    // File is writeable.
    bool       write;
    // File is readable.
    bool       read;
    // Handle is in append mode.
    bool       append;
    // Handle is in non-blocking I/O mode.
    bool       nonblock;

    // Pointer to shared file handle.
    // Directories do not have a shared handle.
    vfs_file_obj_t *obj;
    // Handle number.
    file_t          fileno;
};

// VFS mounted filesystem.
struct vfs {
    // Filesystem vtable.
    vfs_vtable_t       vtable;
    // Copy of mount point.
    char              *mountpoint;
    // Read-only flag.
    bool               readonly;
    // Has a block device.
    bool               has_media;
    // Associated block device.
    fs_media_t         media;
    // Filesystem type.
    fs_driver_t const *driver;
    // Inode number given to the root directory.
    inode_t            inode_root;
    // Filesystem-specific information.
    void              *cookie;
    // Number of currently open files.
    atomic_int         n_open_fd;
    // Root directory file object.
    vfs_file_obj_t    *root_dir_obj;
    // File retured when opening .. at root.
    vfs_file_obj_t    *root_parent_obj;
};

// Identify whether a block device contains a this filesystem.
// Returns 1 if detected, 0 if not, -errno on error.
typedef errno_bool_t (*vfs_detect_t)(fs_media_t *media);

// Filesystem implementation info.
struct fs_driver {
    // Filesystem ID.
    char const         *id;
    // Filesystem vtable.
    vfs_vtable_t const *vtable;
    // Filesystem detection function.
    vfs_detect_t        detect;
    // Filesystem can host device special files.
    bool                supports_devfiles;
};

// Device special file vtable.
struct devfile_vtable {
    // File is seekable.
    bool seekable;
    // [optional] File descriptor opened.
    errno_t (*open_fd)(void *cookie, vfs_file_desc_t *desc);
    // [optional] File descriptor closed.
    errno_t (*close_fd)(void *cookie, vfs_file_desc_t *desc);
    // [optional] File object opened.
    errno_t (*open_obj)(void *cookie, vfs_file_obj_t *fobj);
    // [optional] File object opened.
    errno_t (*close_obj)(void *cookie, vfs_file_obj_t *fobj);
    // File read.
    fileoff_t (*read)(void *cookie, vfs_file_obj_t *fobj, fileoff_t pos, fileoff_t len, void *data);
    // File write.
    fileoff_t (*write)(void *cookie, vfs_file_obj_t *fobj, fileoff_t pos, fileoff_t len, void const *data);
};

// Device special file data.
struct devfile {
    // Device file vtable.
    devfile_vtable_t const *vtable;
    // Cookie for `vtable`.
    void                   *cookie;
    // Paired device, if any.
    device_t               *device;
};

// Dirent cache entry.
struct dentcache {
    // Reference count.
    atomic_int              refcount;
    // Last used time; set to -1 when unused.
    // If not -1, `refcount` has an additional share that will keep it present.
    // It is valid to open a file with this dirent iff `last_used != -1`.
    _Atomic(timestamp_us_t) last_used;
    // Parent dirent; NULL for VFS root.
    dentcache_t            *parent;
    // Mountpoint for VFS root directories not mounted at the root.
    dentcache_t            *mountpoint;
    // Map mutex.
    mutex_t                 mtx;
    // Map of child entries.
    map_t                   children;
    // This dirent.
    dirent_t                dirent;
};

// Push the refcount on a `dentcache_t`.
static inline void dentcache_push_ref(dentcache_t *ent) {
    atomic_fetch_add(&ent->refcount, 1);
}
// Pop the refcount on a `dentcache_t`.
void dentcache_pop_ref(dentcache_t *ent);

// Declare a filesystem driver.
#define FS_DRIVER_DECL(ident) __attribute__((used, section(".fsdrivers"))) static fs_driver_t const ident
