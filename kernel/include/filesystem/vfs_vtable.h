
// SPDX-License-Identifier: MIT

#pragma once

#include "filesystem/vfs_internal.h"



// Try to mount a ramfs filesystem.
typedef bool (*vfs_mount_t)(badge_err_t *ec, vfs_t *vfs);
// Unmount a ramfs filesystem.
typedef void (*vfs_umount_t)(vfs_t *vfs);

// Insert a new file into the given directory.
// If the file already exists, does nothing.
typedef void (*vfs_create_file_t)(badge_err_t *ec, vfs_t *vfs, vfs_file_obj_t *dir, char const *name, size_t name_len);
// Insert a new directory into the given directory.
// If the file already exists, does nothing.
typedef void (*vfs_create_dir_t)(badge_err_t *ec, vfs_t *vfs, vfs_file_obj_t *dir, char const *name, size_t name_len);
// Unlink a file from the given directory.
// If the file is currently open, the file object for it is provided in `file`.
// If this is the last reference to an inode, the inode is deleted.
typedef void (*vfs_unlink_t)(
    badge_err_t *ec, vfs_t *vfs, vfs_file_obj_t *dir, char const *name, size_t name_len, vfs_file_obj_t *file
);
// Remove a directory if it is empty.
// If the directory is currently open, the file object for it is provided in `file`.
// If this is the last reference to an inode, the inode is deleted.
typedef void (*vfs_rmdir_t)(
    badge_err_t *ec, vfs_t *vfs, vfs_file_obj_t *dir, char const *name, size_t name_len, vfs_file_obj_t *file
);
// Create a new hard link from one path to another relative to their respective dirs.
// Fails if `old_path` names a directory.
typedef void (*vfs_link_t)(
    badge_err_t    *ec,
    vfs_t          *vfs,
    vfs_file_obj_t *old_obj,
    vfs_file_obj_t *new_dir,
    char const     *new_name,
    size_t          new_name_len
);
// Create a new symbolic link from one path to another, the latter relative to a dir handle.
typedef void (*vfs_symlink_t)(
    badge_err_t    *ec,
    vfs_t          *vfs,
    char const     *target_path,
    size_t          target_path_len,
    vfs_file_obj_t *link_dir,
    char const     *link_name,
    size_t          link_name_len
);
// Create a new named FIFO at a path relative to a dir handle.
typedef void (*vfs_mkfifo_t)(badge_err_t *ec, vfs_t *vfs, vfs_file_obj_t *dir, char const *name, size_t name_len);

// Atomically read all directory entries and cache them into the directory handle.
// Refer to `dirent_t` for the structure of the cache.
typedef dirent_list_t (*vfs_dir_read_t)(badge_err_t *ec, vfs_t *vfs, vfs_file_obj_t *dir);
// Atomically read the directory entry with the matching name.
// Returns true if the entry was found.
typedef bool (*vfs_dir_find_ent_t)(
    badge_err_t *ec, vfs_t *vfs, vfs_file_obj_t *dir, dirent_t *ent, char const *name, size_t name_len
);

// Stat a file object.
typedef void (*vfs_stat_t)(badge_err_t *ec, vfs_t *vfs, vfs_file_obj_t *file, stat_t *stat);

// Open a file handle for the root directory.
typedef void (*vfs_root_open_t)(badge_err_t *ec, vfs_t *vfs, vfs_file_obj_t *file);
// Open a file for reading and/or writing.
typedef void (*vfs_file_open_t)(
    badge_err_t *ec, vfs_t *vfs, vfs_file_obj_t *dir, vfs_file_obj_t *file, char const *name, size_t name_len
);
// Close a file opened by `vfs_ramfs_file_open`.
// Only raises an error if `file` is an invalid file descriptor.
typedef void (*vfs_file_close_t)(badge_err_t *ec, vfs_t *vfs, vfs_file_obj_t *file);
// Read bytes from a file.
typedef void (*vfs_file_read_t)(
    badge_err_t *ec, vfs_t *vfs, vfs_file_obj_t *file, fileoff_t offset, uint8_t *readbuf, fileoff_t readlen
);
// Write bytes from a file.
typedef void (*vfs_file_write_t)(
    badge_err_t *ec, vfs_t *vfs, vfs_file_obj_t *file, fileoff_t offset, uint8_t const *writebuf, fileoff_t writelen
);
// Change the length of a file opened by `vfs_ramfs_file_open`.
typedef void (*vfs_file_resize_t)(badge_err_t *ec, vfs_t *vfs, vfs_file_obj_t *file, fileoff_t new_size);

// Commit all pending writes on a file to disk.
// The filesystem, if it does caching, must always sync everything to disk at once.
typedef void (*vfs_file_flush_t)(badge_err_t *ec, vfs_t *vfs, vfs_file_obj_t *file);
// Commit all pending writes to disk.
// The filesystem, if it does caching, must always sync everything to disk at once.
typedef void (*vfs_flush_t)(badge_err_t *ec, vfs_t *vfs);



typedef struct {
    vfs_mount_t        mount;
    vfs_umount_t       umount;
    vfs_create_file_t  create_file;
    vfs_create_dir_t   create_dir;
    vfs_unlink_t       unlink;
    vfs_rmdir_t        rmdir;
    vfs_link_t         link;
    vfs_symlink_t      symlink;
    vfs_mkfifo_t       mkfifo;
    vfs_dir_read_t     dir_read;
    vfs_dir_find_ent_t dir_find_ent;
    vfs_stat_t         stat;
    vfs_root_open_t    root_open;
    vfs_file_open_t    file_open;
    vfs_file_close_t   file_close;
    vfs_file_read_t    file_read;
    vfs_file_write_t   file_write;
    vfs_file_resize_t  file_resize;
    vfs_file_flush_t   file_flush;
    vfs_flush_t        flush;
} vfs_vtable_t;
