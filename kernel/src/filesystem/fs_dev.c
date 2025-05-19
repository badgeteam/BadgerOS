
// SPDX-FileCopyrightText: 2025 Julian Scheffers <julian@scheffers.net>
// SPDX-FileType: SOURCE
// SPDX-License-Identifier: MIT

#include "filesystem/fs_dev.h"

#include "badge_err.h"



// Try to mount a devtmpfs filesystem.
bool fs_dev_mount(badge_err_t *ec, vfs_t *vfs) {
    badge_err_set_ok(ec);
    return true;
}

// Unmount a devtmpfs filesystem.
void fs_dev_umount(vfs_t *vfs) {
}

// Identify whether a block device contains a devtmpfs filesystem.
// Returns false on error.
bool fs_dev_detect(badge_err_t *ec, blkdev_t *dev) {
    return false;
}


// devtmpfs doesn't allow this; raises ECAUSE_PERM.
void fs_dev_create_file(badge_err_t *ec, vfs_t *vfs, vfs_file_obj_t *dir, char const *name, size_t name_len) {
    badge_err_set(ec, ELOC_FILESYSTEM, ECAUSE_PERM);
}

// devtmpfs doesn't allow this; raises ECAUSE_PERM.
void fs_dev_create_dir(badge_err_t *ec, vfs_t *vfs, vfs_file_obj_t *dir, char const *name, size_t name_len) {
    badge_err_set(ec, ELOC_FILESYSTEM, ECAUSE_PERM);
}

// devtmpfs doesn't allow this; raises ECAUSE_PERM.
void fs_dev_unlink(
    badge_err_t *ec, vfs_t *vfs, vfs_file_obj_t *dir, char const *name, size_t name_len, vfs_file_obj_t *file
) {
    badge_err_set(ec, ELOC_FILESYSTEM, ECAUSE_PERM);
}

// devtmpfs doesn't allow this; raises ECAUSE_PERM.
void fs_dev_link(
    badge_err_t    *ec,
    vfs_t          *vfs,
    vfs_file_obj_t *old_obj,
    vfs_file_obj_t *new_dir,
    char const     *new_name,
    size_t          new_name_len
) {
    badge_err_set(ec, ELOC_FILESYSTEM, ECAUSE_PERM);
}

// devtmpfs doesn't allow this; raises ECAUSE_PERM.
void fs_dev_symlink(
    badge_err_t    *ec,
    vfs_t          *vfs,
    char const     *target_path,
    size_t          target_path_len,
    vfs_file_obj_t *link_dir,
    char const     *link_name,
    size_t          link_name_len
) {
    badge_err_set(ec, ELOC_FILESYSTEM, ECAUSE_PERM);
}

// devtmpfs doesn't allow this; raises ECAUSE_PERM.
void fs_dev_mkfifo(badge_err_t *ec, vfs_t *vfs, vfs_file_obj_t *dir, char const *name, size_t name_len) {
    badge_err_set(ec, ELOC_FILESYSTEM, ECAUSE_PERM);
}


// Read all entries from a directory.
dirent_list_t fs_dev_dir_read(badge_err_t *ec, vfs_t *vfs, vfs_file_obj_t *dir) {
}


// Read the directory entry with the matching name.
// Returns true if the entry was found.
bool fs_dev_dir_find_ent(
    badge_err_t *ec, vfs_t *vfs, vfs_file_obj_t *dir, dirent_t *ent, char const *name, size_t name_len
) {
}


// Stat a file object.
void fs_dev_stat(badge_err_t *ec, vfs_t *vfs, vfs_file_obj_t *file, stat_t *stat) {
}


// Open a file handle for the root directory.
void fs_dev_root_open(badge_err_t *ec, vfs_t *vfs, vfs_file_obj_t *file) {
}

// Open a file for reading and/or writing.
void fs_dev_file_open(
    badge_err_t *ec, vfs_t *vfs, vfs_file_obj_t *dir, vfs_file_obj_t *file, char const *name, size_t name_len
) {
}

// Close a file opened by `fs_dev_file_open`.
// Only raises an error if `file` is an invalid file descriptor.
void fs_dev_file_close(badge_err_t *ec, vfs_t *vfs, vfs_file_obj_t *file) {
}

// Read bytes from a file.
void fs_dev_file_read(
    badge_err_t *ec, vfs_t *vfs, vfs_file_obj_t *file, fileoff_t offset, uint8_t *readbuf, fileoff_t readlen
) {
}

// Write bytes from a file.
void fs_dev_file_write(
    badge_err_t *ec, vfs_t *vfs, vfs_file_obj_t *file, fileoff_t offset, uint8_t const *writebuf, fileoff_t writelen
) {
}

// Change the length of a file opened by `fs_dev_file_open`.
void fs_dev_file_resize(badge_err_t *ec, vfs_t *vfs, vfs_file_obj_t *file, fileoff_t new_size) {
    (void)vfs;
    (void)file;
    (void)new_size;
    badge_err_set(ec, ELOC_FILESYSTEM, ECAUSE_NOSPACE);
}


// Commit all pending writes to disk.
// The filesystem, if it does caching, must always sync everything to disk at once.
void fs_dev_flush(badge_err_t *ec, vfs_t *vfs) {
    (void)vfs;
    badge_err_set_ok(ec);
}



static vfs_vtable_t fs_dev_vtable = {
    .mount        = fs_dev_mount,
    .umount       = fs_dev_umount,
    .create_file  = fs_dev_create_file,
    .create_dir   = fs_dev_create_dir,
    .unlink       = fs_dev_unlink,
    .rmdir        = fs_dev_unlink,
    .link         = fs_dev_link,
    .symlink      = fs_dev_symlink,
    .mkfifo       = fs_dev_mkfifo,
    .dir_read     = fs_dev_dir_read,
    .dir_find_ent = fs_dev_dir_find_ent,
    .stat         = fs_dev_stat,
    .root_open    = fs_dev_root_open,
    .file_open    = fs_dev_file_open,
    .file_close   = fs_dev_file_close,
    .file_read    = fs_dev_file_read,
    .file_write   = fs_dev_file_write,
    .file_resize  = fs_dev_file_resize,
    // .file_flush   = fs_dev_file_flush,
    .flush        = fs_dev_flush,
};

FS_DRIVER_DECL(fs_dev_driver) = {
    .id               = "devtmpfs",
    .file_cookie_size = 1,
    .vfs_cookie_size  = 1,
    .vtable           = &fs_dev_vtable,
};
