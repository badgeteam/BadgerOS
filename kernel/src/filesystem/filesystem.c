
// SPDX-License-Identifier: MIT

#include "filesystem.h"

#include "arrays.h"
#include "assertions.h"
#include "badge_strings.h"
#include "filesystem/vfs_fifo.h"
#include "filesystem/vfs_internal.h"
#include "filesystem/vfs_vtable.h"
#include "log.h"
#include "malloc.h"

extern fs_driver_t const __start_fsdrivers[];
extern fs_driver_t const __stop_fsdrivers[];

static mutex_t           files_mtx = MUTEX_T_INIT_SHARED;
static size_t            files_len, files_cap;
static vfs_file_desc_t **files;
static file_t            fileno_ctr;

static mutex_t dirs_mtx = MUTEX_T_INIT_SHARED;

typedef struct {
    vfs_file_obj_t *parent;
    vfs_file_obj_t *file;
    char const     *filename;
    size_t          filename_len;
} walk_t;

static bool  root_mounted;
static vfs_t root_fs;



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
static int vfs_file_desc_id_search(void const *a_ptr, void const *b_ptr) {
    vfs_file_desc_t const *a = *(void *const *)a_ptr;
    int                    b = (int)(ptrdiff_t)b_ptr;
    if (a->fileno < b) {
        return -1;
    } else if (a->fileno > b) {
        return 1;
    } else {
        return 0;
    }
}

// Walk the filesystem over a certain path.
// Opens the target file or directory if it exists.
static walk_t walk(badge_err_t *ec, vfs_file_obj_t *dirfd, char const *path, size_t path_len, bool no_follow_symlink) {
    walk_t out = {0};
    if (path[0] == '/') {
        // TODO: chroot support?
        dirfd            = root_fs.root_dir_obj;
        out.file         = vfs_file_dup(dirfd);
        out.filename     = "/";
        out.filename_len = 1;
    }
    dirfd = vfs_file_dup(dirfd);

    while (path_len) {
        if (path[0] == '/' && (!dirfd || dirfd->type != FILETYPE_DIR)) {
            // Not a directory.
            if (out.parent) {
                vfs_file_drop_ref(ec, out.parent);
                out.parent = NULL;
                if (badge_err_is_ok(ec)) {
                    badge_err_set(ec, ELOC_FILESYSTEM, ECAUSE_IS_FILE);
                }
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
            vfs_file_drop_ref(ec, out.parent);
            if (!badge_err_is_ok(ec)) {
                vfs_file_drop_ref(NULL, dirfd);
                out.parent = NULL;
                out.file   = NULL;
                return out;
            }
        }
        out.parent = dirfd;
        out.file   = vfs_file_open(ec, dirfd, path, delim);
        dirfd      = out.file;

        path     += delim;
        path_len -= delim;
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
static void fd_drop_ref(badge_err_t *ec, vfs_file_desc_t *fd) {
    int prev = atomic_fetch_sub(&fd->refcount, 1);
    if (prev == 1) {
        // All refs dropped; close FD.
        vfs_file_drop_ref(ec, fd->obj);
        free(fd);
    } else {
        badge_err_set_ok(ec);
    }
}

// Helper to get FD pointer from file number and increase refcount.
static vfs_file_desc_t *get_fd_ptr(badge_err_t *ec, file_t fileno) {
    mutex_acquire_shared(&files_mtx, TIMESTAMP_US_MAX);
    array_binsearch_t res =
        array_binsearch(files, sizeof(void *), files_len, (void *)(ptrdiff_t)fileno, vfs_file_desc_id_search);
    vfs_file_desc_t *fd = NULL;
    if (res.found) {
        badge_err_set_ok(ec);
        fd = files[res.index];
        fd_take_ref(fd);
    } else {
        badge_err_set(ec, ELOC_FILESYSTEM, ECAUSE_BAD_FD);
    }
    mutex_release_shared(&files_mtx);
    return fd;
}

// Helper to get FD pointer from file number and increase refcount.
// Only accepts files.
static vfs_file_desc_t *get_file_fd_ptr(badge_err_t *ec, file_t fileno) {
    vfs_file_desc_t *fd = get_fd_ptr(ec, fileno);
    if (fd && fd->obj->type == FILETYPE_DIR) {
        fd_drop_ref(ec, fd);
        if (badge_err_is_ok(ec)) {
            badge_err_set(ec, ELOC_FILESYSTEM, ECAUSE_IS_DIR);
        }
        return NULL;
    } else {
        badge_err_set_ok(ec);
    }
    return fd;
}

// Helper to get FD pointer from file number and increase refcount.
// Only accepts dirs.
static vfs_file_desc_t *get_dir_fd_ptr(badge_err_t *ec, file_t fileno) {
    vfs_file_desc_t *fd = get_fd_ptr(ec, fileno);
    if (fd && fd->obj->type != FILETYPE_DIR) {
        fd_drop_ref(ec, fd);
        if (badge_err_is_ok(ec)) {
            badge_err_set(ec, ELOC_FILESYSTEM, ECAUSE_IS_FILE);
        }
        return NULL;
    } else {
        badge_err_set_ok(ec);
    }
    return fd;
}



// Filesystem mount helper function.
static bool mount_at(badge_err_t *ec, vfs_t *vfs, blkdev_t *media, char const *type, mountflags_t flags) {
    // Get driver object.
    fs_driver_t const *driver;
    for (driver = __start_fsdrivers; driver != __stop_fsdrivers; driver++) {
        if (driver->id == type || cstr_equals(driver->id, type)) {
            break;
        }
    }
    if (driver == __stop_fsdrivers) {
        logkf(LOG_ERROR, "Unsupported FS type: ", type);
        badge_err_set(ec, ELOC_FILESYSTEM, ECAUSE_UNAVAIL);
        return false;
    }

    vfs->driver    = driver;
    vfs->media     = media;
    vfs->readonly  = flags & MOUNTFLAGS_READONLY;
    vfs->vtable    = *driver->vtable;
    vfs->n_open_fd = 0;

    if (!driver->vtable->mount(ec, vfs)) {
        return false;
    }

    vfs->root_dir_obj = vfs_root_open(ec, vfs);
    if (!vfs->root_dir_obj) {
        driver->vtable->umount(vfs);
        return false;
    }

    return true;
}

// Try to mount a filesystem.
// Some filesystems (like RAMFS) do not use a block device, for which `media` must be NULL.
// Filesystems which do use a block device can often be automatically detected.
void fs_mount(
    badge_err_t *ec, char const *type, blkdev_t *media, file_t at, char const *path, size_t path_len, mountflags_t flags
) {
    badge_err_t ec0 = {0};
    ec              = ec ?: &ec0;

    if (!type) {
        type = fs_detect(ec, media);
        if (!type) {
            logk(LOG_ERROR, "Cannot determine FS type");
            if (badge_err_is_ok(ec)) {
                badge_err_set(ec, ELOC_FILESYSTEM, ECAUSE_UNAVAIL);
            }
            return;
        }
    }

    // Assert the first mounted FS to be the root.
    if (!root_mounted) {
        if (at != FILE_NONE || path_len != 1 || path[0] != '/') {
            logk(LOG_ERROR, "First filesystem mounted must be mounted at /");
            badge_err_set(ec, ELOC_FILESYSTEM, ECAUSE_ILLEGAL);
            return;
        }
        if (mount_at(ec, &root_fs, media, type, flags)) {
            root_mounted = true;
        }
        return;
    }

    // Get handle for relative directory.
    vfs_file_obj_t *at_obj;
    if (at == FILE_NONE) {
        at_obj = vfs_file_dup(root_fs.root_dir_obj);
    } else {
        vfs_file_desc_t *at_fd = get_dir_fd_ptr(ec, at);
        if (!at_fd) {
            return;
        }
        at_obj = vfs_file_dup(at_fd->obj);
        fd_drop_ref(ec, at_fd);
        if (!badge_err_is_ok(ec)) {
            vfs_file_drop_ref(NULL, at_obj);
            return;
        }
    }

    // Mount the filesystem.
    mutex_acquire(&dirs_mtx, TIMESTAMP_US_MAX);
    walk_t res = walk(ec, at_obj, path, path_len, true);
    vfs_file_drop_ref(ec, at_obj);
    if (!badge_err_is_ok(ec)) {
        if (res.file) {
            vfs_file_drop_ref(NULL, res.file);
        }
        if (res.parent) {
            vfs_file_drop_ref(NULL, res.parent);
        }
        mutex_release(&dirs_mtx);
        return;
    }
    if (!res.file) {
        badge_err_set(ec, ELOC_FILESYSTEM, ECAUSE_NOTFOUND);
    } else {
        // TODO: Assert that dir is empty.
        res.file->mounted_fs = calloc(1, sizeof(vfs_t));
        if (res.file->mounted_fs) {
            if (!mount_at(ec, res.file->mounted_fs, media, type, flags)) {
                free(res.file->mounted_fs);
                res.file->mounted_fs = NULL;
            }
        } else {
            badge_err_set(ec, ELOC_FILESYSTEM, ECAUSE_NOMEM);
        }
        vfs_file_drop_ref(ec, res.file);
    }
    if (res.parent) {
        vfs_file_drop_ref(badge_err_is_ok(ec) ? ec : NULL, res.parent);
    }
    mutex_release(&dirs_mtx);
}

// Try to unmount a filesystem.
// May fail if there any any files open on the target filesystem.
void fs_umount(badge_err_t *ec, file_t at, char const *path, size_t path_len) {
    logk(LOG_WARN, "TODO: fs_umount");
}

// Try to identify the filesystem stored in the block device
// Returns `NULL` on error or if the filesystem is unknown.
char const *fs_detect(badge_err_t *ec, blkdev_t *media) {
    for (fs_driver_t const *driver = __start_fsdrivers; driver != __stop_fsdrivers; driver++) {
        if (driver->detect && driver->detect(ec, media)) {
            return driver->id;
        }
    }
    return NULL;
}



// Test whether a path is a canonical path, but not for the existence of the file or directory.
// A canonical path starts with '/' and contains none of the following regex: `(^|/)\.\.?/|//+`
bool fs_is_canonical_path(char const *path, size_t path_len);



// Get file status given file handler or path, optionally following the final symlink.
// If both `fd` and `path` are specified, `fd` is a directory handle to which `path` is relative.
// Otherwise, either `fd` or `path` is used to get the stat info.
// If `follow_link` is false, the last symlink in the path is not followed.
void fs_stat(badge_err_t *ec, file_t fd, char const *path, size_t path_len, bool follow_link, stat_t *stat_out) {
    vfs_file_obj_t *to_stat;

    // Get the file object to stat.
    if (path) {
        // Get handle for relative directory.
        vfs_file_obj_t *at_obj;
        if (fd == FILE_NONE) {
            at_obj = vfs_file_dup(root_fs.root_dir_obj);
        } else {
            vfs_file_desc_t *at_fd = get_dir_fd_ptr(ec, fd);
            if (!at_fd) {
                return;
            }
            at_obj = vfs_file_dup(at_fd->obj);
            fd_drop_ref(ec, at_fd);
        }

        mutex_acquire(&dirs_mtx, TIMESTAMP_US_MAX);

        // Walk the filesystem.
        walk_t res = walk(ec, at_obj, path, path_len, false);
        vfs_file_drop_ref(ec, at_obj);

        if (!res.file) {
            if (badge_err_is_ok(ec)) {
                badge_err_set(ec, ELOC_FILESYSTEM, ECAUSE_NOENT);
            }
        }
        to_stat = res.file;

        mutex_release(&dirs_mtx);

    } else if (fd != FILE_NONE) {
        // Get the file/dir to stat directly from `fd`.
        vfs_file_desc_t *fd_ptr = get_fd_ptr(ec, fd);
        if (!fd_ptr) {
            return;
        }
        to_stat = vfs_file_dup(fd_ptr->obj);
        fd_drop_ref(NULL, fd_ptr);

    } else {
        badge_err_set(ec, ELOC_FILESYSTEM, ECAUSE_PARAM);
    }

    vfs_stat(ec, to_stat, stat_out);
    vfs_file_drop_ref(NULL, to_stat);
}



// Create a new directory relative to a dir handle.
// If `at` is `FILE_NONE`, it is relative to the root dir.
void fs_mkdir(badge_err_t *ec, file_t at, char const *path, size_t path_len) {
    badge_err_t ec0 = {0};
    ec              = ec ?: &ec0;

    if (!root_mounted) {
        badge_err_set(ec, ELOC_FILESYSTEM, ECAUSE_UNAVAIL);
        logk(LOG_ERROR, "Filesystem op run without a filesystem mounted");
        return;
    }

    // Get handle for relative directory.
    vfs_file_obj_t *at_obj;
    if (at == FILE_NONE) {
        at_obj = vfs_file_dup(root_fs.root_dir_obj);
    } else {
        vfs_file_desc_t *at_fd = get_dir_fd_ptr(ec, at);
        if (!at_fd) {
            return;
        }
        at_obj = vfs_file_dup(at_fd->obj);
        fd_drop_ref(ec, at_fd);
        if (!badge_err_is_ok(ec)) {
            return;
        }
    }

    mutex_acquire(&dirs_mtx, TIMESTAMP_US_MAX);
    walk_t res = walk(ec, at_obj, path, path_len, true);
    vfs_file_drop_ref(ec, at_obj);
    if (!badge_err_is_ok(ec)) {
        if (res.file) {
            vfs_file_drop_ref(NULL, res.file);
        }
        if (res.parent) {
            vfs_file_drop_ref(NULL, res.parent);
        }
        mutex_release(&dirs_mtx);
        return;
    }
    if (res.file) {
        badge_err_set(ec, ELOC_FILESYSTEM, ECAUSE_EXISTS);
        vfs_file_drop_ref(ec, res.file);
    } else if (res.parent) {
        vfs_mkdir(ec, res.parent, res.filename, res.filename_len);
    }
    if (res.parent) {
        vfs_file_drop_ref(badge_err_is_ok(ec) ? ec : NULL, res.parent);
    } else {
        badge_err_set(ec, ELOC_FILESYSTEM, ECAUSE_NOENT);
    }
    mutex_release(&dirs_mtx);
}

// Open a directory for reading relative to a dir handle.
// If `at` is `FILE_NONE`, it is relative to the root dir.
file_t fs_dir_open(badge_err_t *ec, file_t at, char const *path, size_t path_len, oflags_t oflags) {
    return fs_open(ec, at, path, path_len, oflags | OFLAGS_DIRECTORY | OFLAGS_READONLY);
}

// Remove a directory, which must be empty, relative to a dir handle.
// If `at` is `FILE_NONE`, it is relative to the root dir.
void fs_rmdir(badge_err_t *ec, file_t at, char const *path, size_t path_len) {
    if (!root_mounted) {
        badge_err_set(ec, ELOC_FILESYSTEM, ECAUSE_UNAVAIL);
        logk(LOG_ERROR, "Filesystem op run without a filesystem mounted");
        return;
    }

    // Get handle for relative directory.
    vfs_file_obj_t *at_obj;
    if (at == FILE_NONE) {
        at_obj = vfs_file_dup(root_fs.root_dir_obj);
    } else {
        vfs_file_desc_t *at_fd = get_dir_fd_ptr(ec, at);
        if (!at_fd) {
            return;
        }
        at_obj = vfs_file_dup(at_fd->obj);
        fd_drop_ref(ec, at_fd);
    }

    mutex_acquire(&dirs_mtx, TIMESTAMP_US_MAX);

    // Walk the filesystem.
    walk_t res = walk(ec, at_obj, path, path_len, false);
    vfs_file_drop_ref(ec, at_obj);

    if (res.file) {
        vfs_rmdir(ec, res.parent, res.filename, res.filename_len);
    } else {
        badge_err_set(ec, ELOC_FILESYSTEM, ECAUSE_NOENT);
    }

    mutex_release(&dirs_mtx);

    // Clean up.
    if (res.file) {
        vfs_file_drop_ref(ec, res.file);
    }
    if (res.parent) {
        vfs_file_drop_ref(ec, res.parent);
    }
}



// Close a directory opened by `fs_dir_open`.
// Only raises an error if `dir` is an invalid directory descriptor.
void fs_dir_close(badge_err_t *ec, file_t dir) {
    fs_close(ec, dir);
}

// Read all entries from a directory.
dirent_list_t fs_dir_read(badge_err_t *ec, file_t dir) {
    vfs_file_desc_t *fd = get_dir_fd_ptr(ec, dir);
    if (!fd) {
        return (dirent_list_t){0};
    }
    mutex_acquire_shared(&fd->obj->mutex, TIMESTAMP_US_MAX);
    dirent_list_t res = vfs_dir_read(ec, fd->obj);
    mutex_release_shared(&fd->obj->mutex);
    return res;
}



// Unlink a file from the given directory relative to a dir handle.
// If `at` is `FILE_NONE`, it is relative to the root dir.
// If this is the last reference to an inode, the inode is deleted.
// Fails if this is a directory.
void fs_unlink(badge_err_t *ec, file_t at, char const *path, size_t path_len) {
    if (!root_mounted) {
        badge_err_set(ec, ELOC_FILESYSTEM, ECAUSE_UNAVAIL);
        logk(LOG_ERROR, "Filesystem op run without a filesystem mounted");
        return;
    }

    // Get handle for relative directory.
    vfs_file_obj_t *at_obj;
    if (at == FILE_NONE) {
        at_obj = vfs_file_dup(root_fs.root_dir_obj);
    } else {
        vfs_file_desc_t *at_fd = get_dir_fd_ptr(ec, at);
        if (!at_fd) {
            return;
        }
        at_obj = vfs_file_dup(at_fd->obj);
        fd_drop_ref(ec, at_fd);
    }

    mutex_acquire(&dirs_mtx, TIMESTAMP_US_MAX);

    // Walk the filesystem.
    walk_t res = walk(ec, at_obj, path, path_len, false);
    vfs_file_drop_ref(ec, at_obj);

    if (res.file) {
        vfs_unlink(ec, res.parent, res.filename, res.filename_len);
    } else {
        badge_err_set(ec, ELOC_FILESYSTEM, ECAUSE_NOENT);
    }

    mutex_release(&dirs_mtx);

    // Clean up.
    if (res.file) {
        vfs_file_drop_ref(ec, res.file);
    }
    if (res.parent) {
        vfs_file_drop_ref(ec, res.parent);
    }
}

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
) {
    if (!root_mounted) {
        badge_err_set(ec, ELOC_FILESYSTEM, ECAUSE_UNAVAIL);
        logk(LOG_ERROR, "Filesystem op run without a filesystem mounted");
        return;
    }

    // Get handle for relative directory.
    vfs_file_obj_t *new_at_obj;
    if (new_at == FILE_NONE) {
        new_at_obj = vfs_file_dup(root_fs.root_dir_obj);
    } else {
        vfs_file_desc_t *new_at_fd = get_dir_fd_ptr(ec, new_at);
        if (!new_at_fd) {
            badge_err_set(ec, ELOC_FILESYSTEM, ECAUSE_PARAM);
            return;
        }
        new_at_obj = vfs_file_dup(new_at_fd->obj);
        fd_drop_ref(ec, new_at_fd);
    }

    vfs_file_obj_t *old_at_obj;
    if (old_at == FILE_NONE) {
        old_at_obj = vfs_file_dup(root_fs.root_dir_obj);
    } else {
        vfs_file_desc_t *old_at_fd = get_dir_fd_ptr(ec, old_at);
        if (!old_at_fd) {
            vfs_file_drop_ref(ec, new_at_obj);
            badge_err_set(ec, ELOC_FILESYSTEM, ECAUSE_PARAM);
            return;
        }
        old_at_obj = vfs_file_dup(old_at_fd->obj);
        fd_drop_ref(ec, old_at_fd);
    }

    mutex_acquire(&dirs_mtx, TIMESTAMP_US_MAX);

    // Walk the filesystem.
    walk_t old_res = walk(ec, old_at_obj, old_path, old_path_len, false);
    vfs_file_drop_ref(ec, old_at_obj);
    walk_t new_res = walk(ec, new_at_obj, new_path, new_path_len, false);
    vfs_file_drop_ref(ec, new_at_obj);

    // Create hardlink.
    if (new_res.file) {
        badge_err_set(ec, ELOC_FILESYSTEM, ECAUSE_EXISTS);
    } else if (!new_res.parent || !old_res.file) {
        badge_err_set(ec, ELOC_FILESYSTEM, ECAUSE_NOENT);
    } else {
        vfs_link(ec, old_res.file, new_res.parent, new_res.filename, new_res.filename_len);
    }

    mutex_release(&dirs_mtx);

    // Clean up.
    if (old_res.file) {
        vfs_file_drop_ref(ec, old_res.file);
    }
    if (old_res.parent) {
        vfs_file_drop_ref(ec, old_res.parent);
    }
    if (new_res.file) {
        vfs_file_drop_ref(ec, new_res.file);
    }
    if (new_res.parent) {
        vfs_file_drop_ref(ec, new_res.parent);
    }
}

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
) {
    if (!root_mounted) {
        badge_err_set(ec, ELOC_FILESYSTEM, ECAUSE_UNAVAIL);
        logk(LOG_ERROR, "Filesystem op run without a filesystem mounted");
        return;
    }

    // Get handle for relative directory.
    vfs_file_obj_t *link_at_obj;
    if (link_at == FILE_NONE) {
        link_at_obj = vfs_file_dup(root_fs.root_dir_obj);
    } else {
        vfs_file_desc_t *at_fd = get_dir_fd_ptr(ec, link_at);
        if (!at_fd) {
            return;
        }
        link_at_obj = vfs_file_dup(at_fd->obj);
        fd_drop_ref(ec, at_fd);
    }

    mutex_acquire(&dirs_mtx, TIMESTAMP_US_MAX);

    // Walk the filesystem.
    walk_t res = walk(ec, link_at_obj, link_path, link_path_len, false);
    vfs_file_drop_ref(ec, link_at_obj);

    // Create symlink.
    if (res.file) {
        badge_err_set(ec, ELOC_FILESYSTEM, ECAUSE_EXISTS);
    } else {
        vfs_symlink(ec, target_path, target_path_len, res.parent, res.filename, res.filename_len);
    }

    mutex_release(&dirs_mtx);

    // Clean up.
    if (res.file) {
        vfs_file_drop_ref(ec, res.file);
    }
    if (res.parent) {
        vfs_file_drop_ref(ec, res.parent);
    }
}

// Create a new named FIFO at a path relative to a dir handle.
// If `at` is `FILE_NONE`, it is relative to the root dir.
void fs_mkfifo(badge_err_t *ec, file_t at, char const *path, size_t path_len) {
    badge_err_t ec0 = {0};
    ec              = ec ?: &ec0;

    if (!root_mounted) {
        badge_err_set(ec, ELOC_FILESYSTEM, ECAUSE_UNAVAIL);
        logk(LOG_ERROR, "Filesystem op run without a filesystem mounted");
        return;
    }

    // Get handle for relative directory.
    vfs_file_obj_t *at_obj;
    if (at == FILE_NONE) {
        at_obj = vfs_file_dup(root_fs.root_dir_obj);
    } else {
        vfs_file_desc_t *at_fd = get_dir_fd_ptr(ec, at);
        if (!at_fd) {
            return;
        }
        at_obj = vfs_file_dup(at_fd->obj);
        fd_drop_ref(ec, at_fd);
        if (!badge_err_is_ok(ec)) {
            return;
        }
    }

    mutex_acquire(&dirs_mtx, TIMESTAMP_US_MAX);
    walk_t res = walk(ec, at_obj, path, path_len, true);
    vfs_file_drop_ref(ec, at_obj);
    if (!badge_err_is_ok(ec)) {
        if (res.file) {
            vfs_file_drop_ref(NULL, res.file);
        }
        if (res.parent) {
            vfs_file_drop_ref(NULL, res.parent);
        }
        mutex_release(&dirs_mtx);
        return;
    }
    if (res.file) {
        badge_err_set(ec, ELOC_FILESYSTEM, ECAUSE_EXISTS);
        vfs_file_drop_ref(ec, res.file);
    } else if (res.parent) {
        vfs_mkfifo(ec, res.parent, res.filename, res.filename_len);
    }
    if (res.parent) {
        vfs_file_drop_ref(badge_err_is_ok(ec) ? ec : NULL, res.parent);
    } else {
        badge_err_set(ec, ELOC_FILESYSTEM, ECAUSE_NOENT);
    }
    mutex_release(&dirs_mtx);
}



// Create a new pipe with one read and one write end.
fs_pipe_t fs_pipe(badge_err_t *ec, int flags) {
    if (flags & ~VALID_OFLAGS_PIPE) {
        badge_err_set(ec, ELOC_FILESYSTEM, ECAUSE_PARAM);
        return (fs_pipe_t){-1, -1};
    }

    // Allocate file descriptors.
    vfs_file_desc_t *reader = calloc(1, sizeof(vfs_file_obj_t));
    if (!reader) {
        badge_err_set(ec, ELOC_FILESYSTEM, ECAUSE_NOMEM);
        return (fs_pipe_t){-1, -1};
    }
    vfs_file_desc_t *writer = calloc(1, sizeof(vfs_file_obj_t));
    if (!writer) {
        free(reader);
        badge_err_set(ec, ELOC_FILESYSTEM, ECAUSE_NOMEM);
        return (fs_pipe_t){-1, -1};
    }

    // Create the actual pipe handles.
    vfs_pipe_t vfs_pipes = vfs_pipe(ec, flags);
    if (!vfs_pipes.reader || !vfs_pipes.writer) {
        assert_dev_drop(!vfs_pipes.reader && !vfs_pipes.writer);
        free(reader);
        free(writer);
        return (fs_pipe_t){-1, -1};
    }

    // Fill in the file descriptor details.
    reader->read     = true;
    reader->refcount = 1;
    reader->obj      = vfs_pipes.reader;
    writer->write    = true;
    writer->refcount = 1;
    writer->obj      = vfs_pipes.writer;


    mutex_acquire(&files_mtx, TIMESTAMP_US_MAX);

    // Insert both into the files array.
    if (!array_lencap_insert(&files, sizeof(void *), &files_len, &files_cap, &reader, files_len)) {
        mutex_release(&files_mtx);
        vfs_file_drop_ref(NULL, reader->obj);
        vfs_file_drop_ref(NULL, writer->obj);
        free(reader);
        free(writer);
        badge_err_set(ec, ELOC_FILESYSTEM, ECAUSE_NOMEM);
        return (fs_pipe_t){-1, -1};
    }
    reader->fileno = fileno_ctr++;

    if (!array_lencap_insert(&files, sizeof(void *), &files_len, &files_cap, &writer, files_len)) {
        files_len--;
        mutex_release(&files_mtx);
        vfs_file_drop_ref(NULL, reader->obj);
        vfs_file_drop_ref(NULL, writer->obj);
        free(reader);
        free(writer);
        badge_err_set(ec, ELOC_FILESYSTEM, ECAUSE_NOMEM);
        return (fs_pipe_t){-1, -1};
    }
    writer->fileno = fileno_ctr++;

    mutex_release(&files_mtx);

    return (fs_pipe_t){
        .reader = reader->fileno,
        .writer = writer->fileno,
    };
}

// Open a file for reading and/or writing relative to a dir handle.
// If `at` is `FILE_NONE`, it is relative to the root dir.
file_t fs_open(badge_err_t *ec, file_t at, char const *path, size_t path_len, oflags_t oflags) {
    badge_err_t ec0 = {0};
    ec              = ec ?: &ec0;

    if (!root_mounted) {
        badge_err_set(ec, ELOC_FILESYSTEM, ECAUSE_UNAVAIL);
        logk(LOG_ERROR, "Filesystem op run without a filesystem mounted");
        return FILE_NONE;
    }

    if ((oflags & OFLAGS_DIRECTORY) && (oflags & ~VALID_OFLAGS_DIRECTORY)) {
        // Invalid flags for opening dir.
        badge_err_set(ec, ELOC_FILESYSTEM, ECAUSE_PARAM);
        return FILE_NONE;
    }
    if (!(oflags & OFLAGS_DIRECTORY) && (oflags & ~VALID_OFLAGS_FILE)) {
        // Invalid flags for opening file.
        badge_err_set(ec, ELOC_FILESYSTEM, ECAUSE_PARAM);
        return FILE_NONE;
    }
    if (!(oflags & OFLAGS_READWRITE)) {
        // Neither read nor write requested.
        badge_err_set(ec, ELOC_FILESYSTEM, ECAUSE_PARAM);
        return FILE_NONE;
    }
    if ((oflags & OFLAGS_EXCLUSIVE) && !(oflags & OFLAGS_CREATE)) {
        // O_EXCL requires O_CREAT.
        badge_err_set(ec, ELOC_FILESYSTEM, ECAUSE_PARAM);
        return FILE_NONE;
    }

    // Get handle for relative directory.
    vfs_file_obj_t *at_obj;
    if (at == FILE_NONE) {
        at_obj = vfs_file_dup(root_fs.root_dir_obj);
    } else {
        vfs_file_desc_t *at_fd = get_dir_fd_ptr(ec, at);
        if (!at_fd) {
            return FILE_NONE;
        }
        at_obj = vfs_file_dup(at_fd->obj);
        // TODO: finish adding error handling here.
        fd_drop_ref(ec, at_fd);
        if (!badge_err_is_ok(ec)) {
            vfs_file_drop_ref(NULL, at_obj);
            return FILE_NONE;
        }
    }

    // Lock dir modifications.
    if (oflags & OFLAGS_CREATE) {
        mutex_acquire(&dirs_mtx, TIMESTAMP_US_MAX);
    } else {
        mutex_acquire_shared(&dirs_mtx, TIMESTAMP_US_MAX);
    }

    // Walk the filesystem.
    walk_t res = walk(ec, at_obj, path, path_len, false);
    vfs_file_drop_ref(ec, at_obj);
    if (!res.file && res.parent && (oflags & OFLAGS_CREATE)) {
        // Create file if OFLAGS_CREATE is set.
        res.file = vfs_mkfile(ec, res.parent, res.filename, res.filename_len);
    }

    if (res.file) {
        atomic_fetch_add(&res.file->vfs->n_open_fd, 1);
    }

    // Unlock dir modifications.
    if (oflags & OFLAGS_CREATE) {
        mutex_release(&dirs_mtx);
    } else {
        mutex_release_shared(&dirs_mtx);
    }

    if (res.parent) {
        vfs_file_drop_ref(ec, res.parent);
    }

    if (!res.file) {
        badge_err_set(ec, ELOC_FILESYSTEM, ECAUSE_NOENT);
        return FILE_NONE;
    }

    if (res.file->type == FILETYPE_FIFO) {
        // Open the FIFO before finishing the handle creation.
        vfs_fifo_open(
            ec,
            res.file->fifo,
            oflags & OFLAGS_NONBLOCK,
            oflags & OFLAGS_READONLY,
            oflags & OFLAGS_WRITEONLY
        );
        if (!badge_err_is_ok(ec)) {
            vfs_file_drop_ref(NULL, res.file);
            return FILE_NONE;
        }
    }

    // Create new file handle.
    vfs_file_desc_t *fd = calloc(1, sizeof(vfs_file_desc_t));
    fd->obj             = res.file;
    fd->read            = oflags & OFLAGS_READONLY;
    fd->write           = oflags & OFLAGS_WRITEONLY;
    fd->append          = oflags & OFLAGS_APPEND;
    fd->nonblock        = oflags & OFLAGS_NONBLOCK;
    fd->refcount        = 1;

    mutex_acquire(&files_mtx, TIMESTAMP_US_MAX);

    // Insert handle into files array.
    if (!array_lencap_insert(&files, sizeof(void *), &files_len, &files_cap, &fd, files_len)) {
        mutex_release(&files_mtx);
        vfs_file_drop_ref(NULL, fd->obj);
        free(fd);
        badge_err_set(ec, ELOC_FILESYSTEM, ECAUSE_NOMEM);
        return FILE_NONE;
    }
    fd->fileno = fileno_ctr++;

    mutex_release(&files_mtx);
    return fd->fileno;
}

// Close a file opened by `fs_open`.
// Only raises an error if `file` is an invalid file descriptor.
void fs_close(badge_err_t *ec, file_t file) {
    mutex_acquire(&files_mtx, TIMESTAMP_US_MAX);
    array_binsearch_t res =
        array_binsearch(files, sizeof(void *), files_len, (void *)(ptrdiff_t)file, vfs_file_desc_id_search);
    if (res.found) {
        vfs_file_desc_t *fd = files[res.index];
        array_lencap_remove(&files, sizeof(void *), &files_len, &files_cap, NULL, res.index);
        if (fd->obj->type == FILETYPE_FIFO) {
            vfs_fifo_close(fd->obj->fifo, fd->read, fd->write);
        }
        mutex_release(&files_mtx);
        fd_drop_ref(ec, fd);
        badge_err_set_ok(ec);
    } else {
        mutex_release(&files_mtx);
        badge_err_set(ec, ELOC_FILESYSTEM, ECAUSE_PARAM);
    }
}

// Read bytes from a file.
// Returns the amount of data successfully read.
fileoff_t fs_read(badge_err_t *ec, file_t file, void *readbuf, fileoff_t readlen) {
    vfs_file_desc_t *fd = get_file_fd_ptr(ec, file);
    if (!fd) {
        return -1;
    }

    if (!fd->read) {
        fd_drop_ref(ec, fd);
        badge_err_set(ec, ELOC_FILESYSTEM, ECAUSE_PERM);
        return -1;
    }

    if (fd->obj->type == FILETYPE_FIFO && fd->nonblock) {
        // Non-blocking FIFO reads will get as much data as is available.
        return vfs_fifo_read(ec, fd->obj->fifo, true, readbuf, readlen);

    } else if (fd->obj->type == FILETYPE_FIFO) {
        // Blocking FIFO reads will wait until at least one byte is read.
        fileoff_t read;
        do {
            read = vfs_fifo_read(ec, fd->obj->fifo, false, readbuf, readlen);
        } while (read == 0);
        return read;

    } else {
        // Regular file reads.
        mutex_acquire_shared(&fd->obj->mutex, TIMESTAMP_US_MAX);
        fileoff_t offset = fd->offset;
        if (offset > fd->obj->size) {
            offset = fd->obj->size;
        }
        if (offset + readlen > fd->obj->size) {
            readlen = fd->obj->size - offset;
        }
        vfs_file_read(ec, fd->obj, offset, readbuf, readlen);
        if (badge_err_is_ok(ec)) {
            fd->offset = offset + readlen;
        }
        mutex_release_shared(&fd->obj->mutex);
    }

    fd_drop_ref(ec, fd);
    return readlen;
}

// Write bytes to a file.
// Returns the amount of data successfully written.
fileoff_t fs_write(badge_err_t *ec, file_t file, void const *writebuf, fileoff_t writelen) {
    if (writelen < 0) {
        badge_err_set(ec, ELOC_FILESYSTEM, ECAUSE_PARAM);
        return -1;
    }
    vfs_file_desc_t *fd = get_file_fd_ptr(ec, file);
    if (!fd) {
        return -1;
    }

    if (!fd->write) {
        fd_drop_ref(ec, fd);
        badge_err_set(ec, ELOC_FILESYSTEM, ECAUSE_PERM);
        return -1;
    }

    badge_err_t ec0 = {0};
    ec              = ec ?: &ec0;

    if (fd->obj->type == FILETYPE_FIFO && fd->nonblock) {
        // Non-blocking FIFO writes send as much data as is possible without blocking.
        return vfs_fifo_write(ec, fd->obj->fifo, true, false, writebuf, writelen);

    } else if (fd->obj->type == FILETYPE_FIFO) {
        // Blocking FIFO writes block until all data is written,
        // Or set the error code to ECAUSE_PIPE_CLOSED if the FIFO closes midway.
        fileoff_t total_written = 0;
        bool      enforce_open  = false;
        while (writelen) {
            fileoff_t written = vfs_fifo_write(ec, fd->obj->fifo, fd->nonblock, enforce_open, writebuf, writelen);
            if (!badge_err_is_ok(ec)) {
                break;
            }
            writebuf       = (uint8_t const *)writebuf + written;
            writelen      -= written;
            total_written += written;
            enforce_open   = true;
        }
        return total_written;

    } else if (fd->append) {
        // Append writes are atomic and require exclusive locking.
        mutex_acquire(&fd->obj->mutex, TIMESTAMP_US_MAX);
        fd->offset       = fd->obj->size;
        fileoff_t newlen = fd->obj->size + writelen;
        if (newlen > fd->obj->size) {
            vfs_file_resize(ec, fd->obj, newlen);
            if (badge_err_is_ok(ec)) {
                vfs_file_write(ec, fd->obj, fd->offset, writebuf, writelen);
                if (badge_err_is_ok(ec)) {
                    fd->offset = fd->obj->size;
                }
            }
        }
        mutex_release(&fd->obj->mutex);
        if (newlen < fd->obj->size) {
            badge_err_set(ec, ELOC_FILESYSTEM, ECAUSE_NOSPACE);
            writelen = -1;
        }

    } else {
        // Non-append writes may still resize the file but are not atomic.
        mutex_acquire_shared(&fd->obj->mutex, TIMESTAMP_US_MAX);
        fileoff_t offset = fd->offset;
        if (offset + writelen >= offset) {
            if (offset > fd->obj->size) {
                offset = fd->obj->size;
            }

            while (offset + writelen > fd->obj->size) {
                // Grow the file.
                mutex_release_shared(&fd->obj->mutex); // TODO: It brokey her.e
                mutex_acquire(&fd->obj->mutex, TIMESTAMP_US_MAX);

                // Mutex was released for a moment, check size again.
                if (offset > fd->obj->size) {
                    offset = fd->obj->size;
                }
                if (offset + writelen > fd->obj->size) {
                    vfs_file_resize(ec, fd->obj, fd->offset + writelen);
                }

                mutex_release(&fd->obj->mutex);
                mutex_acquire_shared(&fd->obj->mutex, TIMESTAMP_US_MAX);
            }

            // Now that we can assume the file is large enough, perform the write.
            if (badge_err_is_ok(ec)) {
                vfs_file_write(ec, fd->obj, fd->offset, writebuf, writelen);
                if (badge_err_is_ok(ec)) {
                    fd->offset = offset + writelen;
                }
            }
        }
        mutex_release_shared(&fd->obj->mutex);
        if (offset + writelen < offset) {
            badge_err_set(ec, ELOC_FILESYSTEM, ECAUSE_NOSPACE);
            writelen = -1;
        }
    }

    fd_drop_ref(ec, fd);
    return writelen;
}

// Get the current offset in the file.
fileoff_t fs_tell(badge_err_t *ec, file_t file) {
    vfs_file_desc_t *fd = get_file_fd_ptr(ec, file);
    if (!fd) {
        return -1;
    }
    if (fd->obj->type == FILETYPE_FIFO) {
        badge_err_set(ec, ELOC_FILESYSTEM, ECAUSE_UNSEEKABLE);
        return -1;
    }
    mutex_acquire_shared(&fd->obj->mutex, TIMESTAMP_US_MAX);
    fileoff_t tmp = fd->offset;
    if (tmp > fd->obj->size) {
        tmp = fd->obj->size;
    }
    mutex_release_shared(&fd->obj->mutex);
    fd_drop_ref(ec, fd);
    return tmp;
}

// Set the current offset in the file.
// Returns the new offset in the file.
fileoff_t fs_seek(badge_err_t *ec, file_t file, fileoff_t off, fs_seek_t seekmode) {
    vfs_file_desc_t *fd = get_file_fd_ptr(ec, file);
    if (!fd) {
        return -1;
    }
    if (fd->obj->type == FILETYPE_FIFO) {
        badge_err_set(ec, ELOC_FILESYSTEM, ECAUSE_UNSEEKABLE);
        return -1;
    }
    mutex_acquire_shared(&fd->obj->mutex, TIMESTAMP_US_MAX);
    if (seekmode == SEEK_END) {
        off += fd->obj->size;
    } else if (seekmode == SEEK_CUR) {
        off += fd->offset;
    }
    if (off < 0) {
        off = 0;
    } else if (off > fd->obj->size) {
        off = fd->obj->size;
    }
    fd->offset = off;
    mutex_release_shared(&fd->obj->mutex);
    fd_drop_ref(ec, fd);
    return off;
}



// Force any write caches to be flushed for a given file.
// If the file is `FILE_NONE`, all open files are flushed.
void fs_flush(badge_err_t *ec, file_t file) {
    vfs_file_desc_t *fd = get_file_fd_ptr(ec, file);
    if (!fd) {
        // TODO.
    } else {
        mutex_acquire(&fd->obj->mutex, TIMESTAMP_US_MAX);
        vfs_file_flush(ec, fd->obj);
        mutex_release(&fd->obj->mutex);
        fd_drop_ref(ec, fd);
    }
}
