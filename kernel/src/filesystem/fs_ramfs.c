
// SPDX-License-Identifier: MIT

#include "filesystem/fs_ramfs.h"

#include "assertions.h"
#include "badge_strings.h"
#include "filesystem.h"
#include "filesystem/fs_ramfs_types.h"
#include "malloc.h"

#define RAMFS(vfs)    (*(fs_ramfs_t *)(vfs)->cookie)
#define RAMFILE(file) ((fs_ramfs_inode_t *)(file)->cookie)

typedef struct {
    int               errno;
    fs_ramfs_inode_t *inode;
} errno_ramino_t;



// File read.
fileoff_t dev_null_read(void *cookie, vfs_file_obj_t *fobj, fileoff_t pos, fileoff_t len, void *data) {
    (void)fobj;
    (void)pos;

    if (cookie) {
        // DEVTMPFS zero.
        mem_set(data, 0, len);
        return len;

    } else {
        // DEVTMPFS null.
        return 0;
    }
}

// File write.
fileoff_t dev_null_write(void *cookie, vfs_file_obj_t *fobj, fileoff_t pos, fileoff_t len, void const *data) {
    (void)cookie;
    (void)fobj;
    (void)pos;
    (void)len;
    (void)data;
    return len;
}



// DEVTMPFS null/zero vtable.
static devfile_vtable_t dev_null_vtable = {
    .read  = dev_null_read,
    .write = dev_null_write,
};



// RAMFS vtable.
static vfs_vtable_t ramfs_vtable = {
    .mount        = fs_ramfs_mount,
    .umount       = fs_ramfs_umount,
    .create_file  = fs_ramfs_create_file,
    .create_dir   = fs_ramfs_create_dir,
    .unlink       = fs_ramfs_unlink,
    .link         = fs_ramfs_link,
    .symlink      = fs_ramfs_symlink,
    .mkfifo       = fs_ramfs_mkfifo,
    .dir_read     = fs_ramfs_dir_read,
    .dir_find_ent = fs_ramfs_dir_find_ent,
    .root_open    = fs_ramfs_root_open,
    .file_open    = fs_ramfs_file_open,
    .file_close   = fs_ramfs_file_close,
    .file_read    = fs_ramfs_file_read,
    .file_write   = fs_ramfs_file_write,
    .file_resize  = fs_ramfs_file_resize,
    .flush        = fs_ramfs_flush,
};

// RAMFS declaration.
FS_DRIVER_DECL(ramfs_driver) = {
    .vtable = &ramfs_vtable,
    .id     = "ramfs",
};

// DEVTMPFS declaration.
FS_DRIVER_DECL(devtmpfs_driver) = {
    .vtable = &ramfs_vtable,
    .id     = "devtmpfs",
};



// Try to resize an inode.
static errno_t resize_inode(vfs_t *vfs, fs_ramfs_inode_t *inode, size_t size) {
    (void)vfs;

    if (inode->cap >= 2 * size) {
        // If capacity is too large, try to save some memory.
        size_t cap = inode->cap / 2;
        void  *mem = realloc(inode->buf, inode->cap / 2);
        if (mem || cap == 0) {
            inode->cap /= 2;
            inode->buf  = mem;
        }
        inode->len = size;
        return 0;

    } else if (inode->cap >= size) {
        // If capacity is large enough, don't bother resizing.
        inode->len = size;
        return 0;

    } else {
        // If the capacity is not large enough, increase the size.
        size_t cap = 1;
        while (cap < size) {
            cap *= 2;
        }
        void *mem = realloc(inode->buf, cap);
        if (mem) {
            inode->cap = cap;
            inode->buf = mem;
            inode->len = size;
            return 0;
        } else {
            return -ENOSPC;
        }
    }
}

// Find an empty inode.
static ptrdiff_t find_inode(vfs_t *vfs) {
    for (size_t i = VFS_RAMFS_INODE_FIRST; i < RAMFS(vfs).inode_list_len; i++) {
        if (!RAMFS(vfs).inode_usage[i])
            return (ptrdiff_t)i;
    }
    return -1;
}

// Decrease the refcount of an inode and delete it if it reaches 0.
static void pop_inode_refcount(vfs_t *vfs, fs_ramfs_inode_t *inode) {
    // TODO: This needs a re-work for correct `unlink` semantics.
    inode->links--;
    if (inode->links == 0 && !inode->open) {
        // Free inode.
        free(inode->buf);
        RAMFS(vfs).inode_usage[inode->inode] = false;
    }
}

// Insert a new directory entry.
static errno_t insert_dirent(vfs_t *vfs, fs_ramfs_inode_t *dir, fs_ramfs_dirent_t *ent) {
    // Allocate space in the directory.
    size_t  pre_size = dir->len;
    errno_t res      = resize_inode(vfs, dir, pre_size + ent->size);
    if (res < 0) {
        return res;
    }

    // Copy to the destination.
    mem_copy(dir->buf + pre_size, ent, ent->size);
    return 0;
}

// Remove a directory entry.
// Takes a pointer to an entry in the directory's buffer.
static void remove_dirent(vfs_t *vfs, fs_ramfs_inode_t *dir, fs_ramfs_dirent_t *ent) {
    size_t off      = (size_t)ent - (size_t)dir->buf;
    size_t ent_size = ent->size;

    // Copy back entries further in.
    mem_copy(dir->buf + off, dir->buf + off + ent_size, dir->len - ent_size);

    // Resize the buffer.
    resize_inode(vfs, dir, dir->len - ent_size);
}

// Find the directory entry of a given filename in a directory.
// Returns a pointer to an entry in the directory's buffer, or NULL if not found.
static fs_ramfs_dirent_t *find_dirent(vfs_t *vfs, fs_ramfs_inode_t *dir, char const *name, size_t name_len) {
    (void)vfs;
    size_t off = 0;
    while (off < dir->len) {
        fs_ramfs_dirent_t *ent = (fs_ramfs_dirent_t *)(dir->buf + off);
        if (ent->name_len == name_len && mem_equals(name, ent->name, name_len)) {
            return ent;
        }
        off += ent->size;
    }
    return NULL;
}

// Insert a new file or directory into the given directory.
// If the file already exists, does nothing.
static errno_ramino_t
    create_file(vfs_t *vfs, fs_ramfs_inode_t *dirptr, char const *name, size_t name_len, filetype_t type) {
    if (name_len > VFS_RAMFS_NAME_MAX) {
        return (errno_ramino_t){-ENAMETOOLONG, NULL};
    }

    // Test whether the file already exists.
    fs_ramfs_dirent_t *existing = find_dirent(vfs, dirptr, name, name_len);
    if (existing) {
        return (errno_ramino_t){-EEXIST, NULL};
    }

    // Find a vacant inode to assign.
    ptrdiff_t inum = find_inode(vfs);
    if (inum == -1) {
        return (errno_ramino_t){-ENOMEM, NULL};
    }

    // Write a directory entry.
    fs_ramfs_dirent_t ent = {
        .size     = offsetof(fs_ramfs_dirent_t, name) + name_len + 1,
        .name_len = name_len,
        .inode    = inum,
    };
    ent.size += (~ent.size + 1) % sizeof(size_t);
    mem_copy(ent.name, name, name_len);
    ent.name[name_len] = 0;

    // Set up inode.
    fs_ramfs_inode_t *iptr = &RAMFS(vfs).inode_list[inum];

    iptr->buf   = NULL;
    iptr->len   = 0;
    iptr->cap   = 0;
    iptr->inode = inum;
    iptr->mode  = (type << VFS_RAMFS_MODE_BIT) | 0777; /* TODO. */
    iptr->links = 1;
    iptr->uid   = 0; /* TODO. */
    iptr->gid   = 0; /* TODO. */

    // Copy into the end of the directory.
    errno_t res = insert_dirent(vfs, dirptr, &ent);
    if (res >= 0) {
        // If successful, mark inode as in use.
        RAMFS(vfs).inode_usage[inum] = true;
    } else {
        iptr = NULL;
    }
    return (errno_ramino_t){res, iptr};
}

// Test whether a directory is empty.
static bool is_dir_empty(fs_ramfs_inode_t *dir) {
    size_t off = 0;
    while (off < dir->len) {
        fs_ramfs_dirent_t *ent = (fs_ramfs_dirent_t *)(dir->buf + off);
        if (!cstr_equals(".", ent->name) && !cstr_equals("..", ent->name)) {
            return false;
        }
        off += ent->size;
    }
    return true;
}



// Try to mount a ramfs filesystem.
errno_t fs_ramfs_mount(vfs_t *vfs) {
    // RAMFS does not use a block device.
    if (vfs->has_media) {
        return -EINVAL;
    }

    vfs->cookie = calloc(1, sizeof(fs_ramfs_t));
    if (!vfs->cookie) {
        return -ENOMEM;
    }

    // TODO: Parameters.
    atomic_store_explicit(&RAMFS(vfs).ram_usage, 0, memory_order_relaxed);
    RAMFS(vfs).ram_limit      = 65536;
    RAMFS(vfs).inode_list_len = !CONFIG_NOMMU ? 1024 : 64;
    RAMFS(vfs).inode_list     = malloc(sizeof(*RAMFS(vfs).inode_list) * RAMFS(vfs).inode_list_len);
    if (!RAMFS(vfs).inode_list) {
        free(vfs->cookie);
        return -ENOMEM;
    }
    RAMFS(vfs).inode_usage = malloc(sizeof(*RAMFS(vfs).inode_usage) * RAMFS(vfs).inode_list_len);
    if (!RAMFS(vfs).inode_usage) {
        free(RAMFS(vfs).inode_list);
        free(vfs->cookie);
        return -ENOMEM;
    }
    vfs->inode_root = VFS_RAMFS_INODE_ROOT;

    // Clear inode usage.
    mem_set(RAMFS(vfs).inode_list, 0, sizeof(*RAMFS(vfs).inode_list) * RAMFS(vfs).inode_list_len);
    mem_set(RAMFS(vfs).inode_usage, false, RAMFS(vfs).inode_list_len - 2);

    // Create root directory.
    RAMFS(vfs).inode_usage[VFS_RAMFS_INODE_ROOT] = true;
    fs_ramfs_inode_t *iptr                       = &RAMFS(vfs).inode_list[VFS_RAMFS_INODE_ROOT];

    iptr->buf   = NULL;
    iptr->len   = 0;
    iptr->cap   = 0;
    iptr->inode = VFS_RAMFS_INODE_ROOT;
    iptr->mode  = (FILETYPE_DIR << VFS_RAMFS_MODE_BIT) | 0777; /* TODO. */
    iptr->links = 1;
    iptr->uid   = 0; /* TODO. */
    iptr->gid   = 0; /* TODO. */

    // Create root `.` and `..` entries.
    fs_ramfs_dirent_t ent = {
        .size     = sizeof(ent) - VFS_RAMFS_NAME_MAX - 1 + sizeof(size_t),
        .inode    = VFS_RAMFS_INODE_ROOT,
        .name_len = 1,
        .name     = {'.', 0},
    };
    errno_t res = insert_dirent(vfs, iptr, &ent);
    if (res < 0) {
        goto populate_failed;
    }

    ent.name_len = 2;
    ent.name[1]  = '.';
    ent.name[2]  = 0;
    res          = insert_dirent(vfs, iptr, &ent);
    if (res < 0) {
        goto populate_failed;
    }

    if (vfs->driver == &devtmpfs_driver) {
        // Create `null` and `zero` entries.
        errno_ramino_t devnode;

        devnode = create_file(vfs, &RAMFS(vfs).inode_list[VFS_RAMFS_INODE_ROOT], "null", 4, FILETYPE_CHR);
        if (devnode.errno < 0) {
            res = devnode.errno;
            goto populate_failed;
        }
        devnode.inode->devfile.vtable = &dev_null_vtable;
        devnode.inode->devfile.cookie = (void *)0;

        devnode = create_file(vfs, &RAMFS(vfs).inode_list[VFS_RAMFS_INODE_ROOT], "zero", 4, FILETYPE_CHR);
        if (devnode.errno < 0) {
            res = devnode.errno;
            goto populate_failed;
        }
        devnode.inode->devfile.vtable = &dev_null_vtable;
        devnode.inode->devfile.cookie = (void *)1;
    }

    return 0;

populate_failed:
    free(iptr->buf);
    free(RAMFS(vfs).inode_list);
    free(RAMFS(vfs).inode_usage);
    free(vfs->cookie);
    return res;
}

// Unmount a ramfs filesystem.
void fs_ramfs_umount(vfs_t *vfs) {
    free(RAMFS(vfs).inode_list);
    free(RAMFS(vfs).inode_usage);
    free(vfs->cookie);
}



// Insert a new file into the given directory.
// If the file already exists, does nothing.
errno_t fs_ramfs_create_file(vfs_t *vfs, vfs_file_obj_t *dir, char const *name, size_t name_len) {
    return create_file(vfs, RAMFILE(dir), name, name_len, FILETYPE_FILE).errno;
}

// Insert a new directory into the given directory.
// If the file already exists, does nothing.
errno_t fs_ramfs_create_dir(vfs_t *vfs, vfs_file_obj_t *dir, char const *name, size_t name_len) {
    // Create new directory.
    errno_ramino_t mkdir_res = create_file(vfs, RAMFILE(dir), name, name_len, FILETYPE_DIR);
    if (mkdir_res.errno < 0) {
        return mkdir_res.errno;
    }
    fs_ramfs_inode_t *iptr = mkdir_res.inode;

    // Write . and .. entries.
    fs_ramfs_dirent_t ent = {
        .size     = sizeof(ent) - VFS_RAMFS_NAME_MAX - 1 + sizeof(size_t),
        .inode    = iptr->inode,
        .name_len = 1,
        .name     = {'.', 0},
    };
    errno_t res = insert_dirent(vfs, iptr, &ent);
    if (res < 0) {
        pop_inode_refcount(vfs, iptr);
        fs_ramfs_dirent_t *ent = find_dirent(vfs, RAMFILE(dir), name, name_len);
        assert_dev_drop(ent != NULL);
        remove_dirent(vfs, RAMFILE(dir), ent);
        return res;
    }

    ent.name_len = 2;
    ent.name[1]  = '.';
    ent.name[2]  = 0;
    ent.inode    = dir->inode;
    res          = insert_dirent(vfs, iptr, &ent);
    if (res < 0) {
        pop_inode_refcount(vfs, iptr);
        fs_ramfs_dirent_t *ent = find_dirent(vfs, RAMFILE(dir), name, name_len);
        assert_dev_drop(ent != NULL);
        remove_dirent(vfs, RAMFILE(dir), ent);
        return res;
    }

    return 0;
}

// Unlink a file from the given directory.
// If the file is currently open, the file object for it is provided in `file`.
errno_t fs_ramfs_unlink(vfs_t *vfs, vfs_file_obj_t *dir, char const *name, size_t name_len, vfs_file_obj_t *file) {
    if (name_len > VFS_RAMFS_NAME_MAX) {
        return -ENAMETOOLONG;
    }

    // The . and .. entries can not be removed.
    if ((name_len == 1 && name[0] == '.') || (name_len == 2 && name[0] == '.' && name[1] == '.')) {
        return -EINVAL;
    }

    // Find the directory entry with the given name.
    fs_ramfs_inode_t  *dirptr = RAMFILE(dir);
    fs_ramfs_dirent_t *ent    = find_dirent(vfs, dirptr, name, name_len);
    if (!ent) {
        return -ENOENT;
    }


    // If it is also a directory, assert that it is empty.
    fs_ramfs_inode_t *iptr = &RAMFS(vfs).inode_list[ent->inode];
    if ((iptr->mode & VFS_RAMFS_MODE_MASK) == FILETYPE_DIR << VFS_RAMFS_MODE_BIT) {
        // Directories that are not empty cannot be removed.
        if (!is_dir_empty(iptr)) {
            return -ENOTEMPTY;
        }
    }

    // Decrease inode refcount.
    pop_inode_refcount(vfs, iptr);

    // Remove directory entry.
    remove_dirent(vfs, dirptr, ent);

    return 0;
}

// Create a new hard link from one path to another relative to their respective dirs.
// Fails if `old_path` names a directory.
errno_t fs_ramfs_link(
    vfs_t *vfs, vfs_file_obj_t *old_obj, vfs_file_obj_t *new_dir, char const *new_name, size_t new_name_len
) {
    if (new_name_len > VFS_RAMFS_NAME_MAX) {
        return -ENAMETOOLONG;
    }

    // Test whether the file already exists.
    fs_ramfs_inode_t  *dirptr   = RAMFILE(new_dir);
    fs_ramfs_dirent_t *existing = find_dirent(vfs, dirptr, new_name, new_name_len);
    if (existing) {
        return -EEXIST;
    }

    // Write a directory entry.
    fs_ramfs_dirent_t ent = {
        .size     = offsetof(fs_ramfs_dirent_t, name) + new_name_len + 1,
        .name_len = new_name_len,
        .inode    = old_obj->inode,
    };
    ent.size += (~ent.size + 1) % sizeof(size_t);
    mem_copy(ent.name, new_name, new_name_len);
    ent.name[new_name_len] = 0;

    // Copy into the end of the directory.
    errno_t res = insert_dirent(vfs, dirptr, &ent);
    if (res >= 0) {
        // If successful, bump refcount.
        RAMFILE(old_obj)->links++;
    }

    return res;
}

// Create a new symbolic link from one path to another, the latter relative to a dir handle.
errno_t fs_ramfs_symlink(
    vfs_t          *vfs,
    char const     *target_path,
    size_t          target_path_len,
    vfs_file_obj_t *link_dir,
    char const     *link_name,
    size_t          link_name_len
) {
    errno_ramino_t inode = create_file(vfs, RAMFILE(link_dir), link_name, link_name_len, FILETYPE_LINK);
    if (inode.errno < 0) {
        return inode.errno;
    }
    if (!resize_inode(vfs, inode.inode, target_path_len)) {
        pop_inode_refcount(vfs, inode.inode);
        return -ENOSPC;
    }
    mem_copy(inode.inode->buf, target_path, target_path_len);
    return 0;
}

// Create a new named FIFO at a path relative to a dir handle.
errno_t fs_ramfs_mkfifo(vfs_t *vfs, vfs_file_obj_t *dir, char const *name, size_t name_len) {
    return create_file(vfs, RAMFILE(dir), name, name_len, FILETYPE_FIFO).errno;
}



// Determine the record length for converting a RAMFS dirent to a BadgerOS dirent.
// Returns the record length for a matching `dirent_t`.
static inline size_t measure_dirent(fs_ramfs_dirent_t *ent) {
    size_t ent_size  = offsetof(dirent_t, name) + ent->name_len + 1;
    ent_size        += (~ent_size + 1) % 8;
    return ent_size;
}

// Convert a RAMFS dirent to a BadgerOS dirent.
// Returns the record length for a matching `dirent_t`.
static inline size_t convert_dirent(vfs_t *vfs, dirent_t *out, fs_ramfs_dirent_t *in) {
    fs_ramfs_inode_t *iptr = &RAMFS(vfs).inode_list[in->inode];

    out->record_len  = offsetof(dirent_t, name) + in->name_len + 1;
    out->record_len += (fileoff_t)((size_t)(~out->record_len + 1) % 8);
    out->inode       = in->inode;
    out->is_dir      = (iptr->mode & VFS_RAMFS_MODE_MASK) == FILETYPE_DIR << VFS_RAMFS_MODE_BIT;
    out->is_symlink  = (iptr->mode & VFS_RAMFS_MODE_MASK) == FILETYPE_LINK << VFS_RAMFS_MODE_BIT;
    out->name_len    = (fileoff_t)in->name_len;
    mem_copy(out->name, in->name, in->name_len + 1);

    return out->record_len;
}

// Read all entries from a directory.
errno_dirent_list_t fs_ramfs_dir_read(vfs_t *vfs, vfs_file_obj_t *dir) {
    dirent_list_t     list = {0};
    size_t            off  = 0;
    fs_ramfs_inode_t *iptr = RAMFILE(dir);

    // Measure required memory.
    size_t cap = 0;
    while (off < iptr->len) {
        fs_ramfs_dirent_t *ent  = (fs_ramfs_dirent_t *)(iptr->buf + off);
        cap                    += measure_dirent(ent);
        off                    += ent->size;
    }

    // Allocate memory.
    void *mem = malloc(cap);
    if (!mem) {
        return (errno_dirent_list_t){-ENOMEM, (dirent_list_t){0}};
    }
    list.mem  = mem;
    list.size = cap;

    // Generate entries.
    size_t out_off = 0;
    off            = 0;
    while (off < iptr->len) {
        fs_ramfs_dirent_t *in  = (fs_ramfs_dirent_t *)(iptr->buf + off);
        dirent_t          *out = (dirent_t *)((size_t)list.mem + out_off);
        convert_dirent(vfs, out, in);
        off     += in->size;
        out_off += out->record_len;
        list.ent_count++;
    }

    return (errno_dirent_list_t){0, list};
}

// Atomically read the directory entry with the matching name.
// Returns 1 if the entry was found, 0 if not, -errno on error.
errno_bool_t fs_ramfs_dir_find_ent(vfs_t *vfs, vfs_file_obj_t *dir, dirent_t *out, char const *name, size_t name_len) {
    fs_ramfs_inode_t  *iptr = RAMFILE(dir);
    fs_ramfs_dirent_t *in   = find_dirent(vfs, iptr, name, name_len);
    if (!in) {
        return 0;
    } else {
        convert_dirent(vfs, out, in);
        return 1;
    }
}



// Stat a file object.
errno_t fs_ramfs_stat(vfs_t *vfs, vfs_file_obj_t *file, stat_t *stat) {
    (void)vfs;
    stat->links = RAMFILE(file)->links;
    stat->gid   = RAMFILE(file)->gid;
    stat->uid   = RAMFILE(file)->uid;
    stat->size  = RAMFILE(file)->len;
    stat->inode = RAMFILE(file)->inode;
    stat->mode  = RAMFILE(file)->mode;
    return 0;
}



// Open a file handle for the root directory.
errno_t fs_ramfs_root_open(vfs_t *vfs, vfs_file_obj_t *file) {
    // Install in shared file handle.
    fs_ramfs_inode_t *iptr = &RAMFS(vfs).inode_list[VFS_RAMFS_INODE_ROOT];
    file->cookie           = iptr;
    file->inode            = VFS_RAMFS_INODE_ROOT;
    file->refcount         = 1;
    file->type             = FILETYPE_DIR;
    file->is_vfs_root      = true;

    iptr->links++;
    return 0;
}

// Open a file for reading and/or writing.
errno_t fs_ramfs_file_open(vfs_t *vfs, vfs_file_obj_t *dir, vfs_file_obj_t *file, char const *name, size_t name_len) {
    // Look up the file in question.
    fs_ramfs_inode_t  *dirptr = RAMFILE(dir);
    fs_ramfs_dirent_t *ent    = find_dirent(vfs, dirptr, name, name_len);
    if (!ent) {
        return -ENOENT;
    }

    // Mark file as open.
    fs_ramfs_inode_t *iptr = &RAMFS(vfs).inode_list[ent->inode];
    iptr->open             = true;

    // Install in shared file handle.
    file->cookie   = iptr;
    file->inode    = iptr->inode;
    file->vfs      = vfs;
    file->refcount = 1;
    file->size     = (fileoff_t)iptr->len;
    file->type     = (iptr->mode & VFS_RAMFS_MODE_MASK) >> VFS_RAMFS_MODE_BIT;
    if (iptr->devfile.vtable) {
        file->devfile = &iptr->devfile;
    }

    return 0;
}

// Close a file opened by `fs_ramfs_file_open`.
errno_t fs_ramfs_file_close(vfs_t *vfs, vfs_file_obj_t *file) {
    fs_ramfs_inode_t *inode = RAMFILE(file);
    if (inode->links == 0) {
        free(inode->buf);
        RAMFS(vfs).inode_usage[inode->inode] = false;
    }
    return 0;
}

// Read bytes from a file.
errno_t fs_ramfs_file_read(vfs_t *vfs, vfs_file_obj_t *file, fileoff_t offset, uint8_t *readbuf, fileoff_t readlen) {
    if (offset < 0) {
        return -EIO;
    }

    fs_ramfs_inode_t *iptr = RAMFILE(file);

    // Bounds check file and read offsets.
    if (offset + readlen > (ptrdiff_t)iptr->len || offset + readlen < offset) {
        return -EIO;
    }

    // Checks passed, return data.
    mem_copy(readbuf, iptr->buf + offset, readlen);

    return 0;
}

// Write bytes from a file.
errno_t fs_ramfs_file_write(
    vfs_t *vfs, vfs_file_obj_t *file, fileoff_t offset, uint8_t const *writebuf, fileoff_t writelen
) {
    if (offset < 0) {
        return -EIO;
    }

    fs_ramfs_inode_t *iptr = RAMFILE(file);

    // Bounds check file and read offsets.
    if (offset + writelen > (ptrdiff_t)iptr->len || offset + writelen < offset) {
        return -EIO;
    }

    // Checks passed, update data.
    mem_copy(iptr->buf + offset, writebuf, writelen);

    return 0;
}

// Change the length of a file opened by `fs_ramfs_file_open`.
errno_t fs_ramfs_file_resize(vfs_t *vfs, vfs_file_obj_t *file, fileoff_t new_size) {
    if (new_size < 0) {
        return -EIO;
    }

    // Attempt to resize the buffer.
    fs_ramfs_inode_t *iptr     = RAMFILE(file);
    fileoff_t         old_size = (fileoff_t)iptr->len;
    errno_t           res      = resize_inode(vfs, iptr, new_size);
    if (res >= 0) {
        file->size = new_size;
        if (new_size > old_size) {
            // Zero out new bits.
            mem_set(iptr->buf + old_size, 0, new_size - old_size);
        }
    }

    return res;
}



// Commit all pending writes to disk.
// The filesystem, if it does caching, must always sync everything to disk at once.
errno_t fs_ramfs_flush(vfs_t *vfs) {
    // RAMFS does not do caching, so flush does nothing.
    (void)vfs;
    return 0;
}
