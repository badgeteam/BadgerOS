
// SPDX-License-Identifier: MIT

#pragma once

#include "filesystem/vfs_internal.h"



/* ==== VFS interface ==== */

// Try to mount a devtmpfs filesystem.
bool fs_dev_mount(badge_err_t *ec, vfs_t *vfs);
// Unmount a devtmpfs filesystem.
void fs_dev_umount(vfs_t *vfs);

// devtmpfs doesn't allow this; raises ECAUSE_PERM.
void fs_dev_create_file(badge_err_t *ec, vfs_t *vfs, vfs_file_obj_t *dir, char const *name, size_t name_len);
// devtmpfs doesn't allow this; raises ECAUSE_PERM.
void fs_dev_create_dir(badge_err_t *ec, vfs_t *vfs, vfs_file_obj_t *dir, char const *name, size_t name_len);
// devtmpfs doesn't allow this; raises ECAUSE_PERM.
void fs_dev_unlink(
    badge_err_t *ec, vfs_t *vfs, vfs_file_obj_t *dir, char const *name, size_t name_len, vfs_file_obj_t *file
);
// devtmpfs doesn't support this; raises ECAUSE_PERM.
void fs_dev_link(
    badge_err_t    *ec,
    vfs_t          *vfs,
    vfs_file_obj_t *old_obj,
    vfs_file_obj_t *new_dir,
    char const     *new_name,
    size_t          new_name_len
);
// devtmpfs doesn't support this; raises ECAUSE_PERM.
void fs_dev_symlink(
    badge_err_t    *ec,
    vfs_t          *vfs,
    char const     *target_path,
    size_t          target_path_len,
    vfs_file_obj_t *link_dir,
    char const     *link_name,
    size_t          link_name_len
);
// devtmpfs doesn't support this; raises ECAUSE_PERM.
void fs_dev_mkfifo(badge_err_t *ec, vfs_t *vfs, vfs_file_obj_t *dir, char const *name, size_t name_len);

// Read all entries from a directory.
dirent_list_t fs_dev_dir_read(badge_err_t *ec, vfs_t *vfs, vfs_file_obj_t *dir);

// Read the directory entry with the matching name.
// Returns true if the entry was found.
bool fs_dev_dir_find_ent(
    badge_err_t *ec, vfs_t *vfs, vfs_file_obj_t *dir, dirent_t *ent, char const *name, size_t name_len
);

// Stat a file object.
void fs_dev_stat(badge_err_t *ec, vfs_t *vfs, vfs_file_obj_t *file, stat_t *stat);

// Open a file handle for the root directory.
void fs_dev_root_open(badge_err_t *ec, vfs_t *vfs, vfs_file_obj_t *file);
// Open a file for reading and/or writing.
void fs_dev_file_open(
    badge_err_t *ec, vfs_t *vfs, vfs_file_obj_t *dir, vfs_file_obj_t *file, char const *name, size_t name_len
);
// Close a file opened by `fs_dev_file_open`.
// Only raises an error if `file` is an invalid file descriptor.
void fs_dev_file_close(badge_err_t *ec, vfs_t *vfs, vfs_file_obj_t *file);
// Read bytes from a file.
void fs_dev_file_read(
    badge_err_t *ec, vfs_t *vfs, vfs_file_obj_t *file, fileoff_t offset, uint8_t *readbuf, fileoff_t readlen
);
// Write bytes from a file.
void fs_dev_file_write(
    badge_err_t *ec, vfs_t *vfs, vfs_file_obj_t *file, fileoff_t offset, uint8_t const *writebuf, fileoff_t writelen
);
// Change the length of a file opened by `fs_dev_file_open`.
void fs_dev_file_resize(badge_err_t *ec, vfs_t *vfs, vfs_file_obj_t *file, fileoff_t new_size);

// Commit all pending writes to disk.
// The filesystem, if it does caching, must always sync everything to disk at once.
void fs_dev_flush(badge_err_t *ec, vfs_t *vfs);
