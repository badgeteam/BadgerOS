
// SPDX-License-Identifier: MIT

#include "filesystem/vfs_internal.h"

#include "arrays.h"
#include "assertions.h"
#include "filesystem.h"
#include "filesystem/vfs_fifo.h"
#include "filesystem/vfs_types.h"
#include "log.h"
#include "malloc.h"
#include "mutex.h"
#include "time.h"



// File objects array mutex.
static mutex_t          objs_mtx = MUTEX_T_INIT_SHARED;
// Array of existing file objects.
static size_t           objs_len, objs_cap;
// Array of existing file objects.
static vfs_file_obj_t **objs;

#define CHECK_EIO(vfs, value)                                                                                          \
    ({                                                                                                                 \
        errno_t check_eio_errno__ = (value);                                                                           \
        if (check_eio_errno__ == -EIO) {                                                                               \
            logkf(LOG_ERROR, "I/O error (in %{cs}); remounting %{cs} as read-only", __FUNCTION__, (vfs)->mountpoint);  \
            (vfs)->readonly = true;                                                                                    \
        }                                                                                                              \
        check_eio_errno__;                                                                                             \
    })



// Open the root directory of the filesystem.
static errno_fobj_t create_root_fobj(vfs_t *vfs) {
    // Allocate memory for the file handle.
    vfs_file_obj_t *obj = calloc(1, sizeof(vfs_file_obj_t));
    if (!obj) {
        return (errno_fobj_t){-ENOMEM, NULL};
    }

    // Fill out common fields.
    mutex_init(&obj->mutex, true);
    obj->vfs      = vfs;
    obj->refcount = 1;

    // Call FS-specific open root.
    errno_t res = CHECK_EIO(vfs, vfs->vtable.root_open(vfs, obj));
    if (res < 0) {
        free(obj);
        return (errno_fobj_t){res, NULL};
    }

    return (errno_fobj_t){0, obj};
}

// Sort by VFS, then by inode.
static int vfs_file_obj_cmp(void const *a_ptr, void const *b_ptr) {
    vfs_file_obj_t const *a = *(void **)a_ptr;
    vfs_file_obj_t const *b = *(void **)b_ptr;

    if ((size_t)a->vfs < (size_t)b->vfs) {
        return -1;
    } else if ((size_t)a->vfs > (size_t)b->vfs) {
        return 1;
    } else if (a->inode < b->inode) {
        return -1;
    } else if (a->inode > b->inode) {
        return 1;
    } else {
        return 0;
    }
}

// Open existing file object.
static vfs_file_obj_t *open_existing(vfs_t *vfs, inode_t inode) {
    vfs_file_obj_t  dummy     = {.vfs = vfs, .inode = inode};
    vfs_file_obj_t *dummy_ptr = &dummy;

    array_binsearch_t res  = array_binsearch(objs, sizeof(void *), objs_len, &dummy_ptr, vfs_file_obj_cmp);
    vfs_file_obj_t   *fobj = NULL;
    if (res.found) {
        fobj = objs[res.index];
        vfs_file_push_ref(objs[res.index]);
    }

    return fobj;
}



// Open the root directory of the filesystem.
errno_fobj_t vfs_root_open(vfs_t *vfs) {
    mutex_acquire(&objs_mtx, TIMESTAMP_US_MAX);

    // Try to get existing file object.
    vfs_file_obj_t *fobj = open_existing(vfs, vfs->inode_root);
    if (fobj) {
        mutex_release(&objs_mtx);
        return (errno_fobj_t){0, fobj};
    }

    // Create new file object.
    errno_fobj_t res = create_root_fobj(vfs);
    if (res.errno < 0) {
        mutex_release(&objs_mtx);
        return res;
    }

    // Insert into file objects list.
    if (!array_lencap_sorted_insert(&objs, sizeof(void *), &objs_len, &objs_cap, &res.fobj, vfs_file_obj_cmp)) {
        vfs->vtable.file_close(vfs, res.fobj);
        free(fobj);
        res.fobj  = NULL;
        res.errno = -ENOMEM;
    }

    mutex_release(&objs_mtx);

    return res;
}


// Create a new empty file.
errno_fobj_t vfs_mkfile(vfs_file_obj_t *dir, char const *name, size_t name_len) {
    if (dir->type != FILETYPE_DIR) {
        return (errno_fobj_t){-ENOTDIR, NULL};
    }
    while (dir->mounted_fs) {
        dir = dir->mounted_fs->root_dir_obj;
    }
    if (dir->vfs->readonly) {
        return (errno_fobj_t){-EROFS, NULL};
    }

    errno_t res = CHECK_EIO(dir->vfs, dir->vfs->vtable.create_file(dir->vfs, dir, name, name_len));
    if (res < 0) {
        return (errno_fobj_t){res, NULL};
    }
    return vfs_file_open(dir, name, name_len);
}

// Insert a new directory into the given directory.
errno_t vfs_mkdir(vfs_file_obj_t *dir, char const *name, size_t name_len) {
    if (dir->type != FILETYPE_DIR) {
        return -ENOTDIR;
    }
    while (dir->mounted_fs) {
        dir = dir->mounted_fs->root_dir_obj;
    }
    if (dir->vfs->readonly) {
        return -EROFS;
    }
    return CHECK_EIO(dir->vfs, dir->vfs->vtable.create_dir(dir->vfs, dir, name, name_len));
}

// Unlink a file from the given directory.
// If this is the last reference to an inode, the inode is deleted.
errno_t vfs_unlink(vfs_file_obj_t *dir, char const *name, size_t name_len) {
    if (dir->type != FILETYPE_DIR) {
        return -ENOTDIR;
    }
    while (dir->mounted_fs) {
        dir = dir->mounted_fs->root_dir_obj;
    }
    if (dir->vfs->readonly) {
        return -EROFS;
    }

    // Find the dirent.
    dirent_t ent;
    errno_t  res = dir->vfs->vtable.dir_find_ent(dir->vfs, dir, &ent, name, name_len);
    if (res < 1) {
        return res ?: -ENOENT;
    }
    if (ent.is_dir) {
        return -EISDIR;
    }

    return CHECK_EIO(
        dir->vfs,
        dir->vfs->vtable.unlink(dir->vfs, dir, name, name_len, open_existing(dir->vfs, ent.inode))
    );
}

// Create a new hard link from one path to another relative to their respective dirs.
// Fails if `old_path` names a directory.
errno_t vfs_link(vfs_file_obj_t *old_obj, vfs_file_obj_t *new_dir, char const *new_name, size_t new_name_len) {
    // No need to check for pipes at `old_obj` because BadgerOS doesn't allow linking nameless files.
    if (new_dir->type != FILETYPE_DIR) {
        return -ENOTDIR;
    }
    while (new_dir->mounted_fs) {
        new_dir = new_dir->mounted_fs->root_dir_obj;
    }
    if (new_dir->vfs->readonly) {
        return -EROFS;
    }

    if (old_obj->type == FILETYPE_DIR) {
        return -EISDIR;
    } else if (old_obj->vfs != new_dir->vfs) {
        return -EXDEV;
    }

    return CHECK_EIO(new_dir->vfs, old_obj->vfs->vtable.link(old_obj->vfs, old_obj, new_dir, new_name, new_name_len));
}

// Create a new symbolic link from one path to another, the latter relative to a dir handle.
errno_t vfs_symlink(
    char const     *target_path,
    size_t          target_path_len,
    vfs_file_obj_t *link_dir,
    char const     *link_name,
    size_t          link_name_len
) {
    if (link_dir->type != FILETYPE_DIR) {
        return -ENOTDIR;
    }
    while (link_dir->mounted_fs) {
        link_dir = link_dir->mounted_fs->root_dir_obj;
    }
    if (link_dir->vfs->readonly) {
        return -EROFS;
    }
    return CHECK_EIO(
        link_dir->vfs,
        link_dir->vfs->vtable.symlink(link_dir->vfs, target_path, target_path_len, link_dir, link_name, link_name_len)
    );
}

// Create a new named FIFO at a path relative to a dir handle.
errno_t vfs_mkfifo(vfs_file_obj_t *dir, char const *name, size_t name_len) {
    if (dir->type != FILETYPE_DIR) {
        return -ENOTDIR;
    }
    while (dir->mounted_fs) {
        dir = dir->mounted_fs->root_dir_obj;
    }
    if (dir->vfs->readonly) {
        return -EROFS;
    }
    return dir->vfs->vtable.mkfifo(dir->vfs, dir, name, name_len);
}

// Make a device special file; only works on certain filesystem types.
errno_t vfs_mkdevfile(vfs_file_obj_t *dir, char const *name, size_t name_len, devfile_t devfile) {
    if (dir->type != FILETYPE_DIR) {
        if (devfile.device) {
            device_pop_ref(devfile.device);
        }
        return -ENOTDIR;
    }
    while (dir->mounted_fs) {
        dir = dir->mounted_fs->root_dir_obj;
    }
    if (dir->vfs->readonly) {
        if (devfile.device) {
            device_pop_ref(devfile.device);
        }
        return -EROFS;
    }
    if (!dir->vfs->driver->supports_devfiles) {
        logkf(LOG_ERROR, "%{cs} does not support device special files", dir->vfs->driver->id);
        if (devfile.device) {
            device_pop_ref(devfile.device);
        }
        return -ENOTSUP;
    }
    errno_t errno = dir->vfs->vtable.mkdevfile(dir, name, name_len, devfile);
    if (errno < 0 && devfile.device) {
        device_pop_ref(devfile.device);
    }
    return CHECK_EIO(dir->vfs, errno);
}

// Remove a directory if it is empty.
errno_t vfs_rmdir(vfs_file_obj_t *dir, char const *name, size_t name_len) {
    if (dir->type != FILETYPE_DIR) {
        return -ENOTDIR;
    }
    while (dir->mounted_fs) {
        dir = dir->mounted_fs->root_dir_obj;
    }
    if (dir->vfs->readonly) {
        return -EROFS;
    }

    // Find the dirent.
    dirent_t ent;
    errno_t  res = CHECK_EIO(dir->vfs, dir->vfs->vtable.dir_find_ent(dir->vfs, dir, &ent, name, name_len));
    if (res < 1) {
        return res ?: -ENOENT;
    }
    if (ent.is_dir) {
        return -EISDIR;
    }

    // Assert that no filesystem is mounted here.
    vfs_file_obj_t *existing = open_existing(dir->vfs, ent.inode);
    if (dir->is_vfs_root) {
        vfs_file_pop_ref(existing);
        return -EBUSY;
    }

    return CHECK_EIO(dir->vfs, dir->vfs->vtable.rmdir(dir->vfs, dir, name, name_len, existing));
}


// Read all entries from a directory.
errno_dirent_list_t vfs_dir_read(vfs_file_obj_t *dir) {
    if (dir->type != FILETYPE_DIR) {
        return (errno_dirent_list_t){-ENOTDIR, (dirent_list_t){0}};
    }
    while (dir->mounted_fs) {
        dir = dir->mounted_fs->root_dir_obj;
    }
    errno_dirent_list_t res = dir->vfs->vtable.dir_read(dir->vfs, dir);
    CHECK_EIO(dir->vfs, res.errno);
    return res;
}

// Read the directory entry with the matching name.
// Returns true if the entry was found.
errno_bool_t vfs_dir_find_ent(vfs_file_obj_t *dir, dirent_t *ent, char const *name, size_t name_len) {
    if (dir->type != FILETYPE_DIR) {
        return -ENOTDIR;
    }
    while (dir->mounted_fs) {
        dir = dir->mounted_fs->root_dir_obj;
    }
    return CHECK_EIO(dir->vfs, dir->vfs->vtable.dir_find_ent(dir->vfs, dir, ent, name, name_len));
}


// Stat a file object.
errno_t vfs_stat(vfs_file_obj_t *file, stat_t *stat_out) {
    while (file->mounted_fs) {
        file = file->mounted_fs->root_dir_obj;
    }
    if (!file->vfs) {
        // Special case: Stat on an unnamed pipe.
        mem_set(stat_out, 0, sizeof(stat_t));
        return 0;
    }
    return CHECK_EIO(file->vfs, file->vfs->vtable.stat(file->vfs, file, stat_out));
}


// Create a new pipe with one read and one write end.
vfs_pipe_t vfs_pipe(int flags) {
    vfs_file_obj_t *fobj = calloc(1, sizeof(vfs_file_obj_t));
    if (!fobj) {
        return (vfs_pipe_t){-ENOMEM, NULL, NULL};
    }
    fobj->refcount = 2;
    fobj->type     = FILETYPE_FIFO;
    fobj->fifo     = vfs_fifo_create();
    if (!fobj->fifo) {
        free(fobj);
        return (vfs_pipe_t){-ENOMEM, NULL, NULL};
    }
    vfs_fifo_open(fobj->fifo, true, true, true);

    return (vfs_pipe_t){0, fobj, fobj};
}

// Open a file or directory for reading and/or writing given parent directory handle.
errno_fobj_t vfs_file_open(vfs_file_obj_t *dir, char const *name, size_t name_len) {
    if (dir->type != FILETYPE_DIR) {
        return (errno_fobj_t){-ENOTDIR, NULL};
    }

    // Current dir entry needs no lookup.
    if (name_len == 1 && name[0] == '.') {
        vfs_file_push_ref(dir);
        return (errno_fobj_t){0, dir};
    }

    // If an FS is mounted here, redirect to its root directory, unless the filename is `..`.
    if (!(name_len == 2 && name[0] == '.' && name[1] == '.')) {
        while (dir->mounted_fs) {
            dir = dir->mounted_fs->root_dir_obj;
        }
    }

    // Find the dirent.
    dirent_t ent;
    errno_t  res = CHECK_EIO(dir->vfs, dir->vfs->vtable.dir_find_ent(dir->vfs, dir, &ent, name, name_len));
    if (res < 1) {
        return (errno_fobj_t){res ?: -ENOENT, NULL};
    }

    // Redirect root directory references to the mountpoint.
    if (ent.inode == dir->vfs->inode_root) {
        vfs_file_push_ref(dir->vfs->root_parent_obj);
        return (errno_fobj_t){0, dir->vfs->root_parent_obj};
    }

    mutex_acquire(&objs_mtx, TIMESTAMP_US_MAX);

    // Try to get existing file object.
    vfs_file_obj_t *fobj = open_existing(dir->vfs, ent.inode);
    if (fobj) {
        mutex_release(&objs_mtx);
        return (errno_fobj_t){0, fobj};
    }

    // Allocate new file object.
    fobj = calloc(1, sizeof(vfs_file_obj_t));
    if (!fobj) {
        goto err0;
    }
    fobj->vfs      = dir->vfs;
    fobj->refcount = 1;
    mutex_init(&fobj->mutex, true);

    // Open new file object.
    res = CHECK_EIO(dir->vfs, dir->vfs->vtable.file_open(dir->vfs, dir, fobj, name, name_len));
    if (res < 0) {
        goto err1;
    }

    if (fobj->devfile) {
        // Device special file opened.
        if (fobj->devfile->vtable->open_obj) {
            res = fobj->devfile->vtable->open_obj(fobj->devfile->cookie, fobj);
            if (res < 0) {
                goto err1;
            }
        }
    } else if (fobj->type == FILETYPE_FIFO) {
        // FIFO object created here, open will be called by the file desc code.
        fobj->fifo = vfs_fifo_create();
        if (!fobj->fifo) {
            res = -ENOMEM;
            goto err1;
        }
    }

    // Insert into file objects list.
    assert_dev_drop(fobj != NULL);
    if (!array_lencap_sorted_insert(&objs, sizeof(void *), &objs_len, &objs_cap, &fobj, vfs_file_obj_cmp)) {
        if (fobj->devfile) {
            if (fobj->devfile->vtable->close_obj) {
                fobj->devfile->vtable->close_obj(fobj->devfile->cookie, fobj);
            }
        } else if (fobj->type == FILETYPE_FIFO) {
            vfs_fifo_destroy(fobj->fifo);
        }
        dir->vfs->vtable.file_close(dir->vfs, fobj);
        free(fobj);
        fobj = NULL;
        res  = -ENOMEM;
    }

    mutex_release(&objs_mtx);

    return (errno_fobj_t){res, fobj};

err1:
    free(fobj);
err0:
    mutex_release(&objs_mtx);
    return (errno_fobj_t){res, NULL};
}

// Duplicate a file or directory handle.
void vfs_file_push_ref(vfs_file_obj_t *orig) {
    atomic_fetch_add(&orig->refcount, 1);
}

// Close a file opened by `vfs_file_open`.
// Only raises an error if `file` is an invalid file descriptor.
void vfs_file_pop_ref(vfs_file_obj_t *fobj) {
    if (atomic_fetch_sub(&fobj->refcount, 1) > 1) {
        // Don't clean up because there's still references.
        return;
    }

    // Remove from file objects list.
    mutex_acquire(&objs_mtx, TIMESTAMP_US_MAX);
    if (atomic_load(&fobj->refcount)) {
        // Don't clean up if the refcount is nonzero again after the mutex was acquired.
        // Prevents race conditions while keeping performance ok.
        mutex_release(&objs_mtx);
        return;
    }
    array_binsearch_t res = array_binsearch(objs, sizeof(void *), objs_len, &fobj, vfs_file_obj_cmp);
    if (res.found) {
        array_lencap_remove(&objs, sizeof(void *), &objs_len, &objs_cap, NULL, res.index);
    } else {
        assert_dev_drop(fobj->vfs == NULL);
    }
    mutex_release(&objs_mtx);

    if (fobj->devfile) {
        // Closing a device special file.
        if (fobj->devfile->vtable->close_obj) {
            fobj->devfile->vtable->close_obj(fobj->devfile->cookie, fobj);
        }
    } else if (fobj->type == FILETYPE_FIFO) {
        // Closing a FIFO or pipe.
        vfs_fifo_destroy(fobj->fifo);
    }
    if (!fobj->vfs) {
        // Special case: An unnamed pipe does not have a filesystem; skip closing there.
        return;
    }

    CHECK_EIO(fobj->vfs, fobj->vfs->vtable.file_close(fobj->vfs, fobj));
}

// Read bytes from a file.
// The entire read succeeds or the entire read fails, never partial read.
errno_t vfs_file_read(vfs_file_obj_t *file, fileoff_t offset, uint8_t *readbuf, fileoff_t readlen) {
    return CHECK_EIO(file->vfs, file->vfs->vtable.file_read(file->vfs, file, offset, readbuf, readlen));
}

// Write bytes to a file.
// If the file is not large enough, it fails.
// The entire write succeeds or the entire write fails, never partial write.
errno_t vfs_file_write(vfs_file_obj_t *file, fileoff_t offset, uint8_t const *writebuf, fileoff_t writelen) {
    if (file->vfs->readonly) {
        return -EROFS;
    }
    return CHECK_EIO(file->vfs, file->vfs->vtable.file_write(file->vfs, file, offset, writebuf, writelen));
}

// Change the length of a file opened by `vfs_file_open`.
errno_t vfs_file_resize(vfs_file_obj_t *file, fileoff_t new_size) {
    if (file->vfs->readonly) {
        return -EROFS;
    }
    return CHECK_EIO(file->vfs, file->vfs->vtable.file_resize(file->vfs, file, new_size));
}


// Commit all pending writes on a file to disk.
// The filesystem, if it does caching, must always sync everything to disk at once.
errno_t vfs_file_flush(vfs_file_obj_t *file) {
    if (file->type == FILETYPE_DIR) {
        return -EISDIR;
    }
    return CHECK_EIO(file->vfs, file->vfs->vtable.file_flush(file->vfs, file));
}

// Commit all pending writes to disk.
// The filesystem, if it does caching, must always sync everything to disk at once.
errno_t vfs_flush(vfs_t *vfs) {
    return CHECK_EIO(vfs, vfs->vtable.flush(vfs));
}
