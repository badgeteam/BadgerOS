
// SPDX-License-Identifier: MIT

#include "arrays.h"
#include "assertions.h"
#include "badge_strings.h"
#include "filesystem/vfs_internal.h"
#include "filesystem/vfs_vtable.h"
#include "log.h"
#include "malloc.h"

extern fs_driver_t const __start_fsdrivers[];
extern fs_driver_t const __stop_fsdrivers[];

static vfs_file_obj_t *root_shared_fd;

static mutex_t           files_mtx = MUTEX_T_INIT_SHARED;
static size_t            files_len, files_cap;
static vfs_file_desc_t **files;
static file_t            fileno_ctr;

static mutex_t dirs_mtx;

typedef struct {
    vfs_file_obj_t *parent;
    vfs_file_obj_t *file;
    char const     *filename;
    size_t          filename_len;
} walk_t;



// Compare file handle by ID.
static int vfs_file_desc_id_cmp(void const *a_ptr, void const *b_ptr) {
    vfs_file_desc_t const *a = *(void *const *)a_ptr;
    vfs_file_desc_t const *b = *(void *const *)b_ptr;
    if (a->fileno < b->fileno) {
        return -1;
    } else if (a->fileno > b->fileno) {
        return 1;
    } else {
        return 0;
    }
}

// Compare file handle by ID.
static int vfs_file_desc_id_search(void const *a_ptr, void const *b) {
    vfs_file_desc_t const *a = *(void *const *)a_ptr;
    int                    b = (int)(ptrdiff_t)b;
    if (a->fileno < b) {
        return -1;
    } else if (a->fileno > b) {
        return 1;
    } else {
        return 0;
    }
}

// Walk the filesystem over a certain path.
static walk_t walk(badge_err_t *ec, vfs_file_obj_t *dirfd, char const *path, size_t path_len, bool no_follow_symlink) {
    if (path[0] == '/') {
        // TODO: chroot support?
        dirfd = root_shared_fd;
    }
    dirfd      = vfs_file_dup(dirfd);
    walk_t out = {0};

    while (path_len) {
        if (path[0] == '/' && (!dirfd || dirfd->type != FILETYPE_DIR)) {
            // Not a directory.
            if (out.parent) {
                vfs_file_close(out.parent);
                out.parent = NULL;
                badge_err_set(ec, ELOC_FILESYSTEM, ECAUSE_IS_FILE);
            }
            return out;
        }

        // Remove duplicate forward slashes.
        while (path[0] == '/') {
            path++;
            path_len--;
            if (!path_len) {
                badge_err_set_ok(ec);
                return out;
            }
        }

        // Find next forward slash, if any.
        ptrdiff_t delim = mem_index(path, path_len, '/');
        if (delim == -1) {
            delim = path_len;
        }
        out.filename     = path;
        out.filename_len = (size_t)delim;

        // TODO: symlink deref (always).

        // Open next file along path.
        if (out.parent) {
            vfs_file_close(out.parent);
        }
        out.parent = dirfd;
        out.file   = vfs_file_open(ec, dirfd, path, delim);
        dirfd      = out.file;
    }

    // TODO: symlink deref (unless `no_follow_symlink`).
    badge_err_set_ok(ec);
    return out;
}

// Increase the FD refcount.
static void fd_take_ref(vfs_file_desc_t *fd) {
    atomic_fetch_add(&fd->refcount, 1);
}

// Decrease the FD refcount.
static void fd_drop_ref(vfs_file_desc_t *fd) {
    int prev = atomic_fetch_sub(&fd->refcount, 1);
    if (prev == 1) {
        // All refs dropped; close FD.
        vfs_file_close(fd->obj);
        free(fd);
    }
}

// Helper to get FD pointer from file number and increase refcount.
static vfs_file_desc_t *get_fd_ptr(badge_err_t *ec, file_t fileno) {
    mutex_acquire_shared(NULL, &files_mtx, TIMESTAMP_US_MAX);
    array_binsearch_t res =
        array_binsearch(files, sizeof(void *), files_len, (void *)(ptrdiff_t)fileno, vfs_file_desc_id_search);
    vfs_file_desc_t *fd = NULL;
    if (res.found) {
        badge_err_set_ok(ec);
        fd = files[res.index];
        fd_take_ref(fd);
    } else {
        badge_err_set(ec, ELOC_FILESYSTEM, ECAUSE_PARAM);
    }
    mutex_release_shared(NULL, &files_mtx);
}



// Try to mount a filesystem.
// Some filesystems (like RAMFS) do not use a block device, for which `media` must be NULL.
// Filesystems which do use a block device can often be automatically detected.
void fs_mount(badge_err_t *ec, char const *type, blkdev_t *media, char const *mountpoint, mountflags_t flags) {
}

// Unmount a filesystem.
// Only raises an error if there isn't a valid filesystem to unmount.
void fs_umount(badge_err_t *ec, char const *mountpoint) {
}

// Try to identify the filesystem stored in the block device
// Returns `NULL` on error or if the filesystem is unknown.
char const *fs_detect(badge_err_t *ec, blkdev_t *media) {
}



// Test whether a path is a canonical path, but not for the existence of the file or directory.
// A canonical path starts with '/' and contains none of the following regex: `\.\.?/|//+`
bool fs_is_canonical_path(char const *path, size_t path_len) {
}



// Create a new directory relative to a dir handle.
// If `at` is `FILE_NONE`, it is relative to the root dir.
// Returns whether the target exists and is a directory.
bool fs_dir_create(badge_err_t *ec, file_t at, char const *path, size_t path_len) {
}

// Open a directory for reading relative to a dir handle.
// If `at` is `FILE_NONE`, it is relative to the root dir.
file_t fs_dir_open(badge_err_t *ec, file_t at, char const *path, size_t path_len, oflags_t oflags) {
}

// Remove a directory, which must be empty, relative to a dir handle.
// If `at` is `FILE_NONE`, it is relative to the root dir.
void fs_dir_remove(badge_err_t *ec, file_t at, char const *path, size_t path_len) {
}



// Close a directory opened by `fs_dir_open`.
// Only raises an error if `dir` is an invalid directory descriptor.
void fs_dir_close(badge_err_t *ec, file_t dir) {
}

// Read the current directory entry.
// Returns whether a directory entry was successfully read.
bool fs_dir_read(badge_err_t *ec, dirent_t *dirent_out, file_t dir) {
}



// Open a file for reading and/or writing relative to a dir handle.
// If `at` is `FILE_NONE`, it is relative to the root dir.
file_t fs_open(badge_err_t *ec, file_t at, char const *path, size_t path_len, oflags_t oflags) {
    if ((oflags & OFLAGS_EXCLUSIVE) && !(oflags & OFLAGS_CREATE)) {
        badge_err_set(ec, ELOC_FILESYSTEM, ECAUSE_PARAM);
        return FILE_NONE;
    }

    // Get handle for relative directory.
    vfs_file_obj_t *at_fd = root_shared_fd;
    if (at != FILE_NONE) {
        mutex_acquire_shared(NULL, &files_mtx, TIMESTAMP_US_MAX);
        array_binsearch_t res =
            array_binsearch(files, sizeof(void *), files_len, (void *)(ptrdiff_t)at, vfs_file_desc_id_search);
        if (!res.found) {
            mutex_release_shared(NULL, &files_mtx);
            badge_err_set(ec, ELOC_FILESYSTEM, ECAUSE_PARAM);
            return FILE_NONE;
        }
        at_fd = files[res.index]->obj;
    }

    // Lock dir modifications.
    if (oflags & OFLAGS_CREATE) {
        mutex_acquire(NULL, &dirs_mtx, TIMESTAMP_US_MAX);
    } else {
        mutex_acquire_shared(NULL, &dirs_mtx, TIMESTAMP_US_MAX);
    }

    // Walk the filesystem.
    walk_t res = walk(ec, at_fd, path, path_len, false);
    if (!res.file && res.parent && (oflags & OFLAGS_CREATE)) {
        // Create file if OFLAGS_CREATE is set.
        res.file = vfs_file_create(ec, res.parent, res.filename, res.filename_len);
    }

    // Unlock dir modifications.
    if (oflags & OFLAGS_CREATE) {
        mutex_release(NULL, &dirs_mtx);
    } else {
        mutex_release_shared(NULL, &dirs_mtx);
    }
    if (at != FILE_NONE) {
        mutex_release_shared(NULL, &files_mtx);
    }

    if (!res.file) {
        vfs_file_close(res.parent);
        return FILE_NONE;
    }

    // Create new file handle.
    vfs_file_desc_t *fd = calloc(1, sizeof(vfs_file_desc_t));
    fd->obj             = res.file;
    fd->read            = oflags & OFLAGS_READONLY;
    fd->write           = oflags & OFLAGS_WRITEONLY;
    atomic_store_explicit(&fd->refcount, 1, memory_order_release);

    mutex_acquire(NULL, &files_mtx, TIMESTAMP_US_MAX);

    // Insert handle into files array.
    if (!array_lencap_sorted_insert(&files, sizeof(void *), &files_len, &files_cap, &fd, vfs_file_desc_id_cmp)) {
        mutex_release(NULL, &files_mtx);
        vfs_file_close(fd->obj);
        free(fd);
        badge_err_set(ec, ELOC_FILESYSTEM, ECAUSE_NOMEM);
        return FILE_NONE;
    }
    fd->fileno = fileno_ctr++;

    mutex_release(NULL, &files_mtx);
    return fd->fileno;
}

// Unlink a file from the given directory relative to a dir handle.
// If `at` is `FILE_NONE`, it is relative to the root dir.
// If this is the last reference to an inode, the inode is deleted.
// Fails if this is a directory.
void fs_unlink(badge_err_t *ec, file_t at, char const *path, size_t path_len) {
}

// Create a new hard link from one path to another relative to their respective dirs.
// If `*_at` is `FILE_NONE`, it is relative to the root dir.
// Fails if `old_path` names a directory.
void fs_link(badge_err_t *ec, file_t old_at, char const *old_path, file_t new_at, char const *new_path) {
}

// Create a new symbolic link from one path to another, the latter relative to a dir handle.
// The `old_path` specifies a path that is relative to the symlink's location.
// If `new_at` is `FILE_NONE`, it is relative to the root dir.
void fs_symlink(badge_err_t *ec, char const *old_path, file_t new_at, char const *new_path) {
}



// Close a file opened by `fs_open`.
// Only raises an error if `file` is an invalid file descriptor.
void fs_close(badge_err_t *ec, file_t file) {
    mutex_acquire(NULL, &files_mtx, TIMESTAMP_US_MAX);
    array_binsearch_t res =
        array_binsearch(files, sizeof(void *), files_len, (void *)(ptrdiff_t)file, vfs_file_desc_id_search);
    if (res.found) {
        vfs_file_desc_t *fd = files[res.index];
        array_lencap_remove(&files, sizeof(void *), &files_len, &files_cap, NULL, res.index);
        mutex_release(NULL, &files_mtx);
        fd_drop_ref(fd);
        badge_err_set_ok(ec);
    } else {
        mutex_release(NULL, &files_mtx);
        badge_err_set(ec, ELOC_FILESYSTEM, ECAUSE_PARAM);
    }
}

// Read bytes from a file.
// Returns the amount of data successfully read.
fileoff_t fs_read(badge_err_t *ec, file_t file, void *readbuf, fileoff_t readlen) {
    vfs_file_desc_t *fd = get_fd_ptr(ec, file);
    if (!fd) {
        return;
    }
    if (fd->obj->type == FILETYPE_DIR) {
        badge_err_set(ec, ELOC_FILESYSTEM, ECAUSE_ILLEGAL);
        return -1;
    } else {
        mutex_acquire_shared(NULL, &fd->obj->mutex, TIMESTAMP_US_MAX);
        // TODO: Support for stream files (pipes, chardevs, sockets, fifos).
        if (fd->offset + readlen > fd->obj->size) {
            readlen = fd->obj->size - fd->offset;
        }
        vfs_file_read(ec, fd->obj, fd->offset, readbuf, readlen);
        mutex_release_shared(NULL, &fd->obj->mutex);
    }
    fd_drop_ref(fd);
    return readlen;
}

// Write bytes to a file.
// Returns the amount of data successfully written.
fileoff_t fs_write(badge_err_t *ec, file_t file, void const *writebuf, fileoff_t writelen) {
}

// Get the current offset in the file.
fileoff_t fs_tell(badge_err_t *ec, file_t file) {
}

// Set the current offset in the file.
// Returns the new offset in the file.
fileoff_t fs_seek(badge_err_t *ec, file_t file, fileoff_t off, fs_seek_t seekmode) {
}



// Force any write caches to be flushed for a given file.
// If the file is `FILE_NONE`, all open files are flushed.
void fs_flush(badge_err_t *ec, file_t file) {
}
