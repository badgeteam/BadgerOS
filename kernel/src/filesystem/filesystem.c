
// SPDX-License-Identifier: MIT

#include "filesystem.h"

#include "arrays.h"
#include "assertions.h"
#include "badge_strings.h"
#include "device/devtmpfs.h"
#include "errno.h"
#include "filesystem/vfs_fifo.h"
#include "filesystem/vfs_internal.h"
#include "filesystem/vfs_types.h"
#include "filesystem/vfs_vtable.h"
#include "log.h"
#include "malloc.h"
#include "mutex.h"
#include "time.h"

extern fs_driver_t const __start_fsdrivers[];
extern fs_driver_t const __stop_fsdrivers[];

static mutex_t           files_mtx = MUTEX_T_INIT_SHARED;
static size_t            files_len, files_cap;
static vfs_file_desc_t **files;
static file_t            fileno_ctr;

static mutex_t dirs_mtx = MUTEX_T_INIT_SHARED;

typedef struct {
    int             errno;
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
static walk_t walk(vfs_file_obj_t *dirfd, char const *path, size_t path_len, bool no_follow_symlink) {
    walk_t out = {0};
    if (path[0] == '/') {
        // TODO: chroot support?
        vfs_file_push_ref(dirfd);
        dirfd            = root_fs.root_dir_obj;
        out.file         = dirfd;
        out.filename     = "/";
        out.filename_len = 1;
    }
    vfs_file_push_ref(dirfd);

    while (path_len) {
        if (path[0] == '/' && (!dirfd || dirfd->type != FILETYPE_DIR)) {
            // Not a directory.
            if (out.parent) {
                vfs_file_pop_ref(out.parent);
                out.parent = NULL;
                out.errno  = -ENOTDIR;
            }
            return out;
        }

        // Remove duplicate forward slashes.
        while (path[0] == '/') {
            path++;
            path_len--;
            if (!path_len) {
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
            vfs_file_pop_ref(out.parent);
        }
        out.parent         = dirfd;
        errno_fobj_t o_res = vfs_file_open(dirfd, path, delim);
        out.file           = o_res.fobj;
        dirfd              = out.file;

        path     += delim;
        path_len -= delim;
    }

    // TODO: symlink deref (unless `no_follow_symlink`).
    out.errno = 0;
    return out;
}

// Increase the FD refcount.
static void fd_push_ref(vfs_file_desc_t *fd) {
    atomic_fetch_add(&fd->refcount, 1);
}

// Decrease the FD refcount.
static void fd_pop_ref(vfs_file_desc_t *fd) {
    int prev = atomic_fetch_sub(&fd->refcount, 1);
    if (prev == 1) {
        // All refs dropped; close FD.
        vfs_file_pop_ref(fd->obj);
        free(fd);
    }
}

// Helper to get FD pointer from file number and increase refcount.
static errno_fd_t get_fd_ptr(file_t fileno) {
    mutex_acquire_shared(&files_mtx, TIMESTAMP_US_MAX);
    array_binsearch_t res =
        array_binsearch(files, sizeof(void *), files_len, (void *)(ptrdiff_t)fileno, vfs_file_desc_id_search);
    errno_fd_t fd = {0};
    if (res.found) {
        fd.fd = files[res.index];
        fd_push_ref(fd.fd);
    } else {
        fd.errno = -EBADF;
    }
    mutex_release_shared(&files_mtx);
    return fd;
}

// Helper to get FD pointer from file number and increase refcount.
// Only accepts files.
static errno_fd_t get_file_fd_ptr(file_t fileno) {
    errno_fd_t fd = get_fd_ptr(fileno);
    if (fd.fd && fd.fd->obj->type == FILETYPE_DIR) {
        fd_pop_ref(fd.fd);
        fd.errno = -EISDIR;
        fd.fd    = NULL;
        return fd;
    }
    return fd;
}

// Helper to get FD pointer from file number and increase refcount.
// Only accepts dirs.
static errno_fd_t get_dir_fd_ptr(file_t fileno) {
    errno_fd_t fd = get_fd_ptr(fileno);
    if (fd.fd && fd.fd->obj->type != FILETYPE_DIR) {
        fd_pop_ref(fd.fd);
        fd.errno = -ENOTDIR;
        fd.fd    = NULL;
        return fd;
    }
    return fd;
}



// Filesystem mount helper function.
static errno_t mount_at(vfs_t *vfs, fs_media_t *media, char const *type, mountflags_t flags) {
    // Get driver object.
    fs_driver_t const *driver;
    for (driver = __start_fsdrivers; driver != __stop_fsdrivers; driver++) {
        if (driver->id == type || cstr_equals(driver->id, type)) {
            break;
        }
    }
    if (driver == __stop_fsdrivers) {
        logkf(LOG_ERROR, "Unsupported FS type: ", type);
        return -EINVAL;
    }

    vfs->driver    = driver;
    vfs->media     = media;
    vfs->readonly  = flags & MOUNTFLAGS_READONLY;
    vfs->vtable    = *driver->vtable;
    vfs->n_open_fd = 0;

    errno_t res = driver->vtable->mount(vfs);
    if (res < 0) {
        return res;
    }

    errno_fobj_t fobj = vfs_root_open(vfs);
    if (fobj.errno < 0) {
        driver->vtable->umount(vfs);
        return fobj.errno;
    }
    vfs->root_dir_obj = fobj.fobj;

    return 0;
}

// Try to mount a filesystem.
// Some filesystems (like RAMFS) do not use a block device, for which `media` must be NULL.
// Filesystems which do use a block device can often be automatically detected.
errno_t
    fs_mount(char const *type, fs_media_t *media, file_t at, char const *path, size_t path_len, mountflags_t flags) {
    if (!type) {
        errno_ptr_t type_res = fs_detect(media);
        if (!type) {
            logk(LOG_ERROR, "Cannot determine FS type");
            if (type_res.errno >= 0) {
                return -ENOENT;
            }
            return type_res.errno;
        }
    }

    // Assert the first mounted FS to be the root.
    if (!root_mounted) {
        if (at != FILE_NONE || path_len != 1 || path[0] != '/') {
            logk(LOG_ERROR, "First filesystem mounted must be mounted at /");
            return -ENOENT;
        }
        errno_t res = mount_at(&root_fs, media, type, flags);
        if (res >= 0) {
            root_mounted = true;
        }
        return res;
    }

    // Get handle for relative directory.
    vfs_file_obj_t *at_obj;
    if (at == FILE_NONE) {
        vfs_file_push_ref(root_fs.root_dir_obj);
        at_obj = root_fs.root_dir_obj;
    } else {
        errno_fd_t at_fd = get_dir_fd_ptr(at);
        if (at_fd.errno < 0) {
            return at_fd.errno;
        }
        at_obj = at_fd.fd->obj;
        vfs_file_push_ref(at_fd.fd->obj);
        fd_pop_ref(at_fd.fd);
    }

    // Mount the filesystem.
    mutex_acquire(&dirs_mtx, TIMESTAMP_US_MAX);
    walk_t res = walk(at_obj, path, path_len, true);
    vfs_file_pop_ref(at_obj);
    if (res.errno < 0) {
        if (res.file) {
            vfs_file_pop_ref(res.file);
        }
        if (res.parent) {
            vfs_file_pop_ref(res.parent);
        }
        mutex_release(&dirs_mtx);
        return res.errno;
    }
    errno_t errno = 0;
    if (!res.file) {
        errno = -ENOENT;
    } else {
        // TODO: Assert that dir is empty.
        res.file->mounted_fs = calloc(1, sizeof(vfs_t));
        if (res.file->mounted_fs) {
            errno = mount_at(res.file->mounted_fs, media, type, flags);
            if (errno < 0) {
                free(res.file->mounted_fs);
                res.file->mounted_fs = NULL;
            }
        } else {
            vfs_file_pop_ref(res.file);
            errno = -ENOMEM;
        }
    }
    if (res.parent) {
        vfs_file_pop_ref(res.parent);
    }
    mutex_release(&dirs_mtx);

    if (errno == 0 && cstr_equals(type, "devtmpfs")) {
        device_devtmpfs_mounted(fs_dir_open(at, path, path_len, 0));
    }

    return errno;
}

// Try to unmount a filesystem.
// May fail if there any any files open on the target filesystem.
errno_t fs_umount(file_t at, char const *path, size_t path_len) {
    logk(LOG_WARN, "TODO: fs_umount");
    return -ENOTSUP;
}

// Try to identify the filesystem stored in the block device
// Returns `NULL` on error or if the filesystem is unknown.
errno_ptr_t fs_detect(fs_media_t *media) {
    for (fs_driver_t const *driver = __start_fsdrivers; driver != __stop_fsdrivers; driver++) {
        if (!driver->detect) {
            continue;
        }
        errno_t errno = driver->detect(media);
        if (errno != 0) {
            return (errno_ptr_t){(void *)driver->id, errno};
        }
    }
    return (errno_ptr_t){0};
}



// Get the real path of a filename.
// Returns a heap-allocated string on success.
errno_ptr_t fs_realpath(file_t at, char const *path, size_t path_len);



// Get file status given file handler or path, optionally following the final symlink.
// If both `fd` and `path` are specified, `fd` is a directory handle to which `path` is relative.
// Otherwise, either `fd` or `path` is used to get the stat info.
// If `follow_link` is false, the last symlink in the path is not followed.
errno_t fs_stat(file_t fd, char const *path, size_t path_len, bool follow_link, stat_t *stat_out) {
    vfs_file_obj_t *to_stat = NULL;

    // Get the file object to stat.
    if (path) {
        // Get handle for relative directory.
        vfs_file_obj_t *at_obj;
        if (fd == FILE_NONE) {
            vfs_file_push_ref(root_fs.root_dir_obj);
            at_obj = root_fs.root_dir_obj;
        } else {
            errno_fd_t at_fd = get_dir_fd_ptr(fd);
            if (at_fd.errno < 0) {
                return at_fd.errno;
            }
            at_obj = at_fd.fd->obj;
            vfs_file_push_ref(at_fd.fd->obj);
            fd_pop_ref(at_fd.fd);
        }

        mutex_acquire(&dirs_mtx, TIMESTAMP_US_MAX);

        // Walk the filesystem.
        walk_t res = walk(at_obj, path, path_len, false);
        vfs_file_pop_ref(at_obj);

        if (!res.file) {
            if (res.errno >= 0) {
                res.errno = -ENOENT;
            }
            mutex_release(&dirs_mtx);
            return res.errno;
        }
        to_stat = res.file;

        mutex_release(&dirs_mtx);

    } else if (fd != FILE_NONE) {
        // Get the file/dir to stat directly from `fd`.
        errno_fd_t fd_ptr = get_fd_ptr(fd);
        if (fd_ptr.errno < 0) {
            return fd_ptr.errno;
        }
        to_stat = fd_ptr.fd->obj;
        vfs_file_push_ref(fd_ptr.fd->obj);
        fd_pop_ref(fd_ptr.fd);

    } else {
        return -EINVAL;
    }

    vfs_stat(to_stat, stat_out);
    vfs_file_pop_ref(to_stat);

    return 0;
}



// Create a new directory relative to a dir handle.
// If `at` is `FILE_NONE`, it is relative to the root dir.
errno_t fs_mkdir(file_t at, char const *path, size_t path_len) {
    if (!root_mounted) {
        logk(LOG_ERROR, "Filesystem op run without a filesystem mounted");
        return -EAGAIN;
    }

    // Get handle for relative directory.
    vfs_file_obj_t *at_obj;
    if (at == FILE_NONE) {
        vfs_file_push_ref(root_fs.root_dir_obj);
        at_obj = root_fs.root_dir_obj;
    } else {
        errno_fd_t at_fd = get_dir_fd_ptr(at);
        if (at_fd.errno < 0) {
            return at_fd.errno;
        }
        at_obj = at_fd.fd->obj;
        vfs_file_push_ref(at_fd.fd->obj);
        fd_pop_ref(at_fd.fd);
    }

    mutex_acquire(&dirs_mtx, TIMESTAMP_US_MAX);
    walk_t res = walk(at_obj, path, path_len, true);
    vfs_file_pop_ref(at_obj);
    if (res.file) {
        res.errno = -EEXIST;
        vfs_file_pop_ref(res.file);
    } else if (res.parent) {
        res.errno = vfs_mkdir(res.parent, res.filename, res.filename_len);
    }
    if (res.parent) {
        vfs_file_pop_ref(res.parent);
    } else {
        res.errno = -ENOENT;
    }
    mutex_release(&dirs_mtx);

    return res.errno;
}

// Open a directory for reading relative to a dir handle.
// If `at` is `FILE_NONE`, it is relative to the root dir.
file_t fs_dir_open(file_t at, char const *path, size_t path_len, oflags_t oflags) {
    return fs_open(at, path, path_len, oflags | OFLAGS_DIRECTORY | OFLAGS_READONLY);
}

// Remove a directory, which must be empty, relative to a dir handle.
// If `at` is `FILE_NONE`, it is relative to the root dir.
errno_t fs_rmdir(file_t at, char const *path, size_t path_len) {
    if (!root_mounted) {
        logk(LOG_ERROR, "Filesystem op run without a filesystem mounted");
        return -EAGAIN;
    }

    // Get handle for relative directory.
    vfs_file_obj_t *at_obj;
    if (at == FILE_NONE) {
        at_obj = root_fs.root_dir_obj;
        vfs_file_push_ref(root_fs.root_dir_obj);
    } else {
        errno_fd_t at_fd = get_dir_fd_ptr(at);
        if (at_fd.errno < 0) {
            return at_fd.errno;
        }
        at_obj = at_fd.fd->obj;
        vfs_file_push_ref(at_fd.fd->obj);
        fd_pop_ref(at_fd.fd);
    }

    mutex_acquire(&dirs_mtx, TIMESTAMP_US_MAX);

    // Walk the filesystem.
    walk_t res = walk(at_obj, path, path_len, false);
    vfs_file_pop_ref(at_obj);

    if (res.file) {
        res.errno = vfs_rmdir(res.parent, res.filename, res.filename_len);
    } else if (res.errno >= 0) {
        res.errno = -ENOENT;
    }

    mutex_release(&dirs_mtx);

    // Clean up.
    if (res.file) {
        vfs_file_pop_ref(res.file);
    }
    if (res.parent) {
        vfs_file_pop_ref(res.parent);
    }

    return res.errno;
}



// Close a directory opened by `fs_dir_open`.
// Only raises an error if `dir` is an invalid directory descriptor.
errno_t fs_dir_close(file_t dir) {
    return fs_close(dir);
}

// Read all entries from a directory.
errno_dirent_list_t fs_dir_read(file_t dir) {
    errno_fd_t fd = get_dir_fd_ptr(dir);
    if (fd.errno < 0) {
        return (errno_dirent_list_t){.errno = fd.errno};
    }
    mutex_acquire_shared(&fd.fd->obj->mutex, TIMESTAMP_US_MAX);
    errno_dirent_list_t res = vfs_dir_read(fd.fd->obj);
    mutex_release_shared(&fd.fd->obj->mutex);
    return res;
}



// Unlink a file from the given directory relative to a dir handle.
// If `at` is `FILE_NONE`, it is relative to the root dir.
// If this is the last reference to an inode, the inode is deleted.
// Fails if this is a directory.
errno_t fs_unlink(file_t at, char const *path, size_t path_len) {
    if (!root_mounted) {
        logk(LOG_ERROR, "Filesystem op run without a filesystem mounted");
        return -EAGAIN;
    }

    // Get handle for relative directory.
    vfs_file_obj_t *at_obj;
    if (at == FILE_NONE) {
        at_obj = root_fs.root_dir_obj;
        vfs_file_push_ref(root_fs.root_dir_obj);
    } else {
        errno_fd_t at_fd = get_dir_fd_ptr(at);
        if (at_fd.errno < 0) {
            return at_fd.errno;
        }
        at_obj = at_fd.fd->obj;
        vfs_file_push_ref(at_fd.fd->obj);
        fd_pop_ref(at_fd.fd);
    }

    mutex_acquire(&dirs_mtx, TIMESTAMP_US_MAX);

    // Walk the filesystem.
    walk_t res = walk(at_obj, path, path_len, false);
    vfs_file_pop_ref(at_obj);

    if (res.file) {
        res.errno = vfs_unlink(res.parent, res.filename, res.filename_len);
    } else if (res.errno >= 0) {
        res.errno = -ENOENT;
    }

    mutex_release(&dirs_mtx);

    // Clean up.
    if (res.file) {
        vfs_file_pop_ref(res.file);
    }
    if (res.parent) {
        vfs_file_pop_ref(res.parent);
    }

    return res.errno;
}

// Create a new hard link from one path to another relative to their respective dirs.
// If `*_at` is `FILE_NONE`, it is relative to the root dir.
// Fails if `old_path` names a directory.
errno_t fs_link(
    file_t old_at, char const *old_path, size_t old_path_len, file_t new_at, char const *new_path, size_t new_path_len
) {
    if (!root_mounted) {
        logk(LOG_ERROR, "Filesystem op run without a filesystem mounted");
        return -EAGAIN;
    }

    // Get handle for relative directory.
    vfs_file_obj_t *new_at_obj;
    if (new_at == FILE_NONE) {
        new_at_obj = root_fs.root_dir_obj;
        vfs_file_push_ref(root_fs.root_dir_obj);
    } else {
        errno_fd_t new_at_fd = get_dir_fd_ptr(new_at);
        if (new_at_fd.errno < 0) {
            return new_at_fd.errno;
        }
        new_at_obj = new_at_fd.fd->obj;
        vfs_file_push_ref(new_at_fd.fd->obj);
        fd_pop_ref(new_at_fd.fd);
    }

    vfs_file_obj_t *old_at_obj;
    if (old_at == FILE_NONE) {
        old_at_obj = root_fs.root_dir_obj;
        vfs_file_push_ref(root_fs.root_dir_obj);
    } else {
        errno_fd_t old_fd = get_dir_fd_ptr(old_at);
        if (old_fd.errno < 0) {
            return old_fd.errno;
        }
        old_at_obj = old_fd.fd->obj;
        vfs_file_push_ref(old_fd.fd->obj);
        fd_pop_ref(old_fd.fd);
    }

    mutex_acquire(&dirs_mtx, TIMESTAMP_US_MAX);

    // Walk the filesystem.
    walk_t old_res = walk(old_at_obj, old_path, old_path_len, false);
    vfs_file_pop_ref(old_at_obj);
    walk_t new_res = {0};
    if (old_res.errno >= 0) {
        new_res = walk(new_at_obj, new_path, new_path_len, false);
    }
    vfs_file_pop_ref(new_at_obj);

    // Create hardlink.
    errno_t errno;
    if (old_res.errno < 0) {
        errno = old_res.errno;
    } else if (new_res.errno < 0) {
        errno = new_res.errno;
    } else if (new_res.file) {
        errno = -EEXIST;
    } else if (!new_res.parent || !old_res.file) {
        errno = -ENOENT;
    } else {
        errno = vfs_link(old_res.file, new_res.parent, new_res.filename, new_res.filename_len);
    }

    mutex_release(&dirs_mtx);

    // Clean up.
    if (old_res.file) {
        vfs_file_pop_ref(old_res.file);
    }
    if (old_res.parent) {
        vfs_file_pop_ref(old_res.parent);
    }
    if (new_res.file) {
        vfs_file_pop_ref(new_res.file);
    }
    if (new_res.parent) {
        vfs_file_pop_ref(new_res.parent);
    }

    return errno;
}

// Create a new symbolic link from one path to another, the latter relative to a dir handle.
// The `old_path` specifies a path that is relative to the symlink's location.
// If `new_at` is `FILE_NONE`, it is relative to the root dir.
errno_t fs_symlink(
    char const *target_path, size_t target_path_len, file_t link_at, char const *link_path, size_t link_path_len
) {
    if (!root_mounted) {
        logk(LOG_ERROR, "Filesystem op run without a filesystem mounted");
        return -EAGAIN;
    }

    // Get handle for relative directory.
    vfs_file_obj_t *link_at_obj;
    if (link_at == FILE_NONE) {
        link_at_obj = root_fs.root_dir_obj;
        vfs_file_push_ref(root_fs.root_dir_obj);
    } else {
        errno_fd_t at_fd = get_dir_fd_ptr(link_at);
        if (at_fd.errno < 0) {
            return at_fd.errno;
        }
        link_at_obj = at_fd.fd->obj;
        vfs_file_push_ref(at_fd.fd->obj);
        fd_pop_ref(at_fd.fd);
    }

    mutex_acquire(&dirs_mtx, TIMESTAMP_US_MAX);

    // Walk the filesystem.
    walk_t res = walk(link_at_obj, link_path, link_path_len, false);
    vfs_file_pop_ref(link_at_obj);

    if (res.file) {
        res.errno = vfs_symlink(target_path, target_path_len, res.parent, res.filename, res.filename_len);
    } else if (res.errno >= 0) {
        res.errno = -ENOENT;
    }

    mutex_release(&dirs_mtx);

    // Clean up.
    if (res.file) {
        vfs_file_pop_ref(res.file);
    }
    if (res.parent) {
        vfs_file_pop_ref(res.parent);
    }

    return res.errno;
}

// Create a new named FIFO at a path relative to a dir handle.
// If `at` is `FILE_NONE`, it is relative to the root dir.
errno_t fs_mkfifo(file_t at, char const *path, size_t path_len) {
    if (!root_mounted) {
        logk(LOG_ERROR, "Filesystem op run without a filesystem mounted");
        return -EAGAIN;
    }

    // Get handle for relative directory.
    vfs_file_obj_t *at_obj;
    if (at == FILE_NONE) {
        at_obj = root_fs.root_dir_obj;
        vfs_file_push_ref(root_fs.root_dir_obj);
    } else {
        errno_fd_t at_fd = get_dir_fd_ptr(at);
        if (at_fd.errno < 0) {
            return at_fd.errno;
        }
        at_obj = at_fd.fd->obj;
        vfs_file_push_ref(at_fd.fd->obj);
        fd_pop_ref(at_fd.fd);
    }

    mutex_acquire(&dirs_mtx, TIMESTAMP_US_MAX);

    // Walk the filesystem.
    walk_t res = walk(at_obj, path, path_len, false);
    vfs_file_pop_ref(at_obj);

    if (res.file) {
        res.errno = vfs_mkfifo(res.parent, res.filename, res.filename_len);
    } else if (res.errno >= 0) {
        res.errno = -ENOENT;
    }

    mutex_release(&dirs_mtx);

    // Clean up.
    if (res.file) {
        vfs_file_pop_ref(res.file);
    }
    if (res.parent) {
        vfs_file_pop_ref(res.parent);
    }

    return res.errno;
}

// Make a device special file; only works on certain filesystem types.
errno_t fs_mkdevfile(file_t at, char const *path, size_t path_len, devfile_t devfile) {
    if (!root_mounted) {
        logk(LOG_ERROR, "Filesystem op run without a filesystem mounted");
        return -EAGAIN;
    }

    // Get handle for relative directory.
    vfs_file_obj_t *at_obj;
    if (at == FILE_NONE) {
        at_obj = root_fs.root_dir_obj;
        vfs_file_push_ref(root_fs.root_dir_obj);
    } else {
        errno_fd_t at_fd = get_dir_fd_ptr(at);
        if (at_fd.errno < 0) {
            return at_fd.errno;
        }
        at_obj = at_fd.fd->obj;
        vfs_file_push_ref(at_fd.fd->obj);
        fd_pop_ref(at_fd.fd);
    }

    mutex_acquire(&dirs_mtx, TIMESTAMP_US_MAX);

    // Walk the filesystem.
    walk_t res = walk(at_obj, path, path_len, false);
    vfs_file_pop_ref(at_obj);

    if (res.file) {
        res.errno = vfs_mkdevfile(res.parent, res.filename, res.filename_len, devfile);
    } else if (res.errno >= 0) {
        res.errno = -ENOENT;
    }

    mutex_release(&dirs_mtx);

    // Clean up.
    if (res.file) {
        vfs_file_pop_ref(res.file);
    }
    if (res.parent) {
        vfs_file_pop_ref(res.parent);
    }

    return res.errno;
}



// Create a new pipe with one read and one write end.
fs_pipe_t fs_pipe(int flags) {
    if (flags & ~VALID_OFLAGS_PIPE) {
        return (fs_pipe_t){-EINVAL, -1, -1};
    }

    // Allocate file descriptors.
    vfs_file_desc_t *reader = calloc(1, sizeof(vfs_file_obj_t));
    if (!reader) {
        return (fs_pipe_t){-ENOMEM, -1, -1};
    }
    vfs_file_desc_t *writer = calloc(1, sizeof(vfs_file_obj_t));
    if (!writer) {
        free(reader);
        return (fs_pipe_t){-ENOMEM, -1, -1};
    }

    // Create the actual pipe handles.
    vfs_pipe_t vfs_pipes = vfs_pipe(flags);
    if (vfs_pipes.errno < 0) {
        assert_dev_drop(!vfs_pipes.reader && !vfs_pipes.writer);
        free(reader);
        free(writer);
        return (fs_pipe_t){vfs_pipes.errno, -1, -1};
    }
    assert_dev_drop(vfs_pipes.reader && vfs_pipes.writer);

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
        vfs_file_pop_ref(reader->obj);
        vfs_file_pop_ref(writer->obj);
        free(reader);
        free(writer);
        return (fs_pipe_t){-ENOMEM, -1, -1};
    }
    reader->fileno = fileno_ctr++;

    if (!array_lencap_insert(&files, sizeof(void *), &files_len, &files_cap, &writer, files_len)) {
        files_len--;
        mutex_release(&files_mtx);
        vfs_file_pop_ref(reader->obj);
        vfs_file_pop_ref(writer->obj);
        free(reader);
        free(writer);
        return (fs_pipe_t){-ENOMEM, -1, -1};
    }
    writer->fileno = fileno_ctr++;

    mutex_release(&files_mtx);

    return (fs_pipe_t){
        .errno  = 0,
        .reader = reader->fileno,
        .writer = writer->fileno,
    };
}

// Open a file for reading and/or writing relative to a dir handle.
// If `at` is `FILE_NONE`, it is relative to the root dir.
file_t fs_open(file_t at, char const *path, size_t path_len, oflags_t oflags) {
    if (!root_mounted) {
        logk(LOG_ERROR, "Filesystem op run without a filesystem mounted");
        return -EAGAIN;
    }

    if ((oflags & OFLAGS_DIRECTORY) && (oflags & ~VALID_OFLAGS_DIRECTORY)) {
        // Invalid flags for opening dir.
        return -EINVAL;
    }
    if (!(oflags & OFLAGS_DIRECTORY) && (oflags & ~VALID_OFLAGS_FILE)) {
        // Invalid flags for opening file.
        return -EINVAL;
    }
    if (!(oflags & OFLAGS_READWRITE)) {
        // Neither read nor write requested.
        return -EINVAL;
    }
    if ((oflags & OFLAGS_EXCLUSIVE) && !(oflags & OFLAGS_CREATE)) {
        // O_EXCL requires O_CREAT.
        return -EINVAL;
    }

    // Get handle for relative directory.
    vfs_file_obj_t *at_obj;
    if (at == FILE_NONE) {
        at_obj = root_fs.root_dir_obj;
        vfs_file_push_ref(root_fs.root_dir_obj);
    } else {
        errno_fd_t at_fd = get_dir_fd_ptr(at);
        if (at_fd.errno < 0) {
            return at_fd.errno;
        }
        at_obj = at_fd.fd->obj;
        vfs_file_push_ref(at_fd.fd->obj);
        fd_pop_ref(at_fd.fd);
    }

    // Lock dir modifications.
    if (oflags & OFLAGS_CREATE) {
        mutex_acquire(&dirs_mtx, TIMESTAMP_US_MAX);
    } else {
        mutex_acquire_shared(&dirs_mtx, TIMESTAMP_US_MAX);
    }

    // Walk the filesystem.
    walk_t res = walk(at_obj, path, path_len, false);
    vfs_file_pop_ref(at_obj);
    if (!res.file && res.parent && (oflags & OFLAGS_CREATE)) {
        // Create file if OFLAGS_CREATE is set.
        errno_fobj_t fobj = vfs_mkfile(res.parent, res.filename, res.filename_len);
        res.file          = fobj.fobj;
        res.errno         = fobj.errno;
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
        vfs_file_pop_ref(res.parent);
    }

    if (!res.file) {
        return -ENOENT;
    }

    if (!res.file->devfile && res.file->type == FILETYPE_FIFO) {
        // Open the FIFO before finishing the handle creation.
        vfs_fifo_open(res.file->fifo, oflags & OFLAGS_NONBLOCK, oflags & OFLAGS_READONLY, oflags & OFLAGS_WRITEONLY);
    }

    // Create new file handle.
    vfs_file_desc_t *fd = calloc(1, sizeof(vfs_file_desc_t));
    fd->obj             = res.file;
    fd->read            = oflags & OFLAGS_READONLY;
    fd->write           = oflags & OFLAGS_WRITEONLY;
    fd->append          = oflags & OFLAGS_APPEND;
    fd->nonblock        = oflags & OFLAGS_NONBLOCK;
    fd->refcount        = 1;

    if (res.file->devfile && res.file->devfile->vtable->open_fd) {
        // Notify of device file opening after handle creation.
        errno_t devfile_res = res.file->devfile->vtable->open_fd(res.file->devfile->cookie, fd);
        if (devfile_res < 0) {
            free(fd);
            vfs_file_pop_ref(res.file);
            return devfile_res;
        }
    }

    mutex_acquire(&files_mtx, TIMESTAMP_US_MAX);

    // Insert handle into files array.
    if (!array_lencap_insert(&files, sizeof(void *), &files_len, &files_cap, &fd, files_len)) {
        mutex_release(&files_mtx);
        vfs_file_pop_ref(fd->obj);
        free(fd);
        return -ENOMEM;
    }
    fd->fileno = fileno_ctr++;

    mutex_release(&files_mtx);
    return fd->fileno;
}

// Close a file opened by `fs_open`.
// Only raises an error if `file` is an invalid file descriptor.
errno_t fs_close(file_t file) {
    mutex_acquire(&files_mtx, TIMESTAMP_US_MAX);
    array_binsearch_t res =
        array_binsearch(files, sizeof(void *), files_len, (void *)(ptrdiff_t)file, vfs_file_desc_id_search);
    if (res.found) {
        vfs_file_desc_t *fd = files[res.index];
        if (fd->obj->devfile && fd->obj->devfile->vtable->close_fd) {
            fd->obj->devfile->vtable->close_fd(fd->obj->devfile->cookie, fd);
        }
        array_lencap_remove(&files, sizeof(void *), &files_len, &files_cap, NULL, res.index);
        if (!fd->obj->devfile && fd->obj->type == FILETYPE_FIFO) {
            vfs_fifo_close(fd->obj->fifo, fd->read, fd->write);
        }
        mutex_release(&files_mtx);
        fd_pop_ref(fd);
        return 0;

    } else {
        mutex_release(&files_mtx);
        return -EBADF;
    }
}

// Read bytes from a file.
// Returns the amount of data successfully read.
fileoff_t fs_read(file_t file, void *readbuf, fileoff_t readlen) {
    fileoff_t  read = 0;
    errno_fd_t fd   = get_file_fd_ptr(file);
    if (fd.errno < 0) {
        return fd.errno;
    }

    if (!fd.fd->read) {
        fd_pop_ref(fd.fd);
        return -EBADF;
    }

    if (fd.fd->obj->devfile) {
        // Device file read.
        read =
            fd.fd->obj->devfile->vtable->read(fd.fd->obj->devfile->cookie, fd.fd->obj, fd.fd->offset, readlen, readbuf);
        if (read >= 0 && fd.fd->obj->devfile->vtable->seekable) {
            fd.fd->offset += read;
        }

    } else if (fd.fd->obj->type == FILETYPE_FIFO && fd.fd->nonblock) {
        // Non-blocking FIFO reads will get as much data as is available.
        read = vfs_fifo_read(fd.fd->obj->fifo, true, readbuf, readlen);

    } else if (fd.fd->obj->type == FILETYPE_FIFO) {
        // Blocking FIFO reads will wait until at least one byte is read.
        do {
            read = vfs_fifo_read(fd.fd->obj->fifo, false, readbuf, readlen);
        } while (read == 0);

    } else {
        // Regular file reads.
        mutex_acquire_shared(&fd.fd->obj->mutex, TIMESTAMP_US_MAX);
        fileoff_t offset = fd.fd->offset;
        if (offset > fd.fd->obj->size) {
            offset = fd.fd->obj->size;
        }
        if (offset + readlen > fd.fd->obj->size) {
            readlen = fd.fd->obj->size - offset;
        }
        errno_t read_res = vfs_file_read(fd.fd->obj, offset, readbuf, readlen);
        if (read_res >= 0) {
            fd.fd->offset = offset + readlen;
            read          = readlen;
        } else {
            read = read_res;
        }
        mutex_release_shared(&fd.fd->obj->mutex);
    }

    fd_pop_ref(fd.fd);
    return read;
}

// Write bytes to a file.
// Returns the amount of data successfully written.
fileoff_t fs_write(file_t file, void const *writebuf, fileoff_t writelen) {
    // Return value; either -errno or written length.
    fileoff_t written = 0;
    if (writelen < 0) {
        return -EINVAL;
    }
    errno_fd_t fd = get_file_fd_ptr(file);
    if (fd.errno < 0) {
        return fd.errno;
    }

    if (!fd.fd->write) {
        fd_pop_ref(fd.fd);
        return -EBADF;
    }

    if (fd.fd->obj->devfile) {
        // Device file write.
        written = fd.fd->obj->devfile->vtable
                      ->write(fd.fd->obj->devfile->cookie, fd.fd->obj, fd.fd->offset, writelen, writebuf);
        if (written >= 0 && fd.fd->obj->devfile->vtable->seekable) {
            fd.fd->offset += written;
        }

    } else if (fd.fd->obj->type == FILETYPE_FIFO && fd.fd->nonblock) {
        // Non-blocking FIFO writes send as much data as is possible without blocking.
        written = vfs_fifo_write(fd.fd->obj->fifo, true, false, writebuf, writelen);

    } else if (fd.fd->obj->type == FILETYPE_FIFO) {
        // Blocking FIFO writes block until all data is written,
        // Or set the error code to ECAUSE_PIPE_CLOSED if the FIFO closes midway.
        bool enforce_open = false;
        while (writelen) {
            fileoff_t batch_written =
                vfs_fifo_write(fd.fd->obj->fifo, fd.fd->nonblock, enforce_open, writebuf, writelen);
            if (batch_written < 0) {
                // Pipe broken.
                written = -EPIPE;
                break;
            }
            writebuf      = (uint8_t const *)writebuf + batch_written;
            writelen     -= batch_written;
            written      += batch_written;
            enforce_open  = true;
        }

    } else if (fd.fd->append) {
        // Append writes are atomic and require exclusive locking.
        mutex_acquire(&fd.fd->obj->mutex, TIMESTAMP_US_MAX);
        fd.fd->offset    = fd.fd->obj->size;
        fileoff_t newlen = fd.fd->obj->size + writelen;
        if (newlen > fd.fd->obj->size) {
            written = vfs_file_resize(fd.fd->obj, newlen);
            if (written >= 0) {
                written = vfs_file_write(fd.fd->obj, fd.fd->offset, writebuf, writelen);
                if (written >= 0) {
                    fd.fd->offset = fd.fd->obj->size;
                }
            }
        }
        mutex_release(&fd.fd->obj->mutex);
        if (newlen < fd.fd->obj->size) {
            written = -ENOSPC;
        } else if (written >= 0) {
            written = writelen;
        }

    } else {
        // Non-append writes may still resize the file but are not atomic.
        mutex_acquire_shared(&fd.fd->obj->mutex, TIMESTAMP_US_MAX);
        fileoff_t offset = fd.fd->offset;
        if (offset + writelen >= offset) {
            if (offset > fd.fd->obj->size) {
                offset = fd.fd->obj->size;
            }

            while (offset + writelen > fd.fd->obj->size) {
                // Grow the file.
                mutex_release_shared(&fd.fd->obj->mutex); // TODO: It brokey her.e
                mutex_acquire(&fd.fd->obj->mutex, TIMESTAMP_US_MAX);

                // Mutex was released for a moment, check size again.
                if (offset > fd.fd->obj->size) {
                    offset = fd.fd->obj->size;
                }
                if (offset + writelen > fd.fd->obj->size) {
                    written = vfs_file_resize(fd.fd->obj, fd.fd->offset + writelen);
                }

                mutex_release(&fd.fd->obj->mutex);
                mutex_acquire_shared(&fd.fd->obj->mutex, TIMESTAMP_US_MAX);
            }

            // Now that we can assume the file is large enough, perform the write.
            if (written >= 0) {
                written = vfs_file_write(fd.fd->obj, fd.fd->offset, writebuf, writelen);
                if (written >= 0) {
                    fd.fd->offset = offset + writelen;
                }
            }
        }
        mutex_release_shared(&fd.fd->obj->mutex);
        if (offset + writelen < offset) {
            written = -ENOSPC;
        } else if (written >= 0) {
            written = writelen;
        }
    }

    fd_pop_ref(fd.fd);
    return written;
}

// Get the current offset in the file.
fileoff_t fs_tell(file_t file) {
    errno_fd_t fd = get_file_fd_ptr(file);
    if (fd.errno < 0) {
        return fd.errno;
    }
    if (fd.fd->obj->type == FILETYPE_FIFO) {
        fd_pop_ref(fd.fd);
        return -ESPIPE;
    }
    mutex_acquire_shared(&fd.fd->obj->mutex, TIMESTAMP_US_MAX);
    fileoff_t tmp = fd.fd->offset;
    if (tmp > fd.fd->obj->size) {
        tmp = fd.fd->obj->size;
    }
    mutex_release_shared(&fd.fd->obj->mutex);
    fd_pop_ref(fd.fd);
    return tmp;
}

// Set the current offset in the file.
// Returns the new offset in the file.
fileoff_t fs_seek(file_t file, fileoff_t off, fs_seek_t seekmode) {
    errno_fd_t fd = get_file_fd_ptr(file);
    if (!fd.fd) {
        return -1;
    }
    if ((fd.fd->obj->devfile && !fd.fd->obj->devfile->vtable->seekable) || fd.fd->obj->type == FILETYPE_FIFO) {
        fd_pop_ref(fd.fd);
        return -ESPIPE;
    }
    mutex_acquire_shared(&fd.fd->obj->mutex, TIMESTAMP_US_MAX);
    if (seekmode == SEEK_END) {
        off += fd.fd->obj->size;
    } else if (seekmode == SEEK_CUR) {
        off += fd.fd->offset;
    }
    if (off < 0) {
        off = 0;
    } else if (off > fd.fd->obj->size) {
        off = fd.fd->obj->size;
    }
    fd.fd->offset = off;
    mutex_release_shared(&fd.fd->obj->mutex);
    fd_pop_ref(fd.fd);
    return off;
}



// Force any write caches to be flushed for a given file.
// If the file is `FILE_NONE`, all open files are flushed.
errno_t fs_flush(file_t file) {
    errno_fd_t fd = get_file_fd_ptr(file);
    if (fd.errno == -EBADF) {
        // TODO: Sync all files.
        return -ENOTSUP;
    } else if (fd.errno >= 0) {
        mutex_acquire(&fd.fd->obj->mutex, TIMESTAMP_US_MAX);
        errno_t res = vfs_file_flush(fd.fd->obj);
        mutex_release(&fd.fd->obj->mutex);
        fd_pop_ref(fd.fd);
        return res;
    } else {
        return -fd.errno;
    }
}
