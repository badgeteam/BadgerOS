
// SPDX-License-Identifier: MIT

#pragma once

#include "errno.h"
#include "filesystem.h"
#include "filesystem/vfs_types.h"

// Return value of `vfs_pipe_t`.
// This isn't the same single file object to future-proof for bidirectional unnamed pipes.
typedef struct {
    int             errno;
    vfs_file_obj_t *reader;
    vfs_file_obj_t *writer;
} vfs_pipe_t;



// Open the root directory of the filesystem.
errno_fobj_t vfs_root_open(vfs_t *vfs);

// Create a new empty file.
errno_fobj_t vfs_mkfile(vfs_file_obj_t *dir, char const *name, size_t name_len);
// Insert a new directory into the given directory.
errno_t      vfs_mkdir(vfs_file_obj_t *dir, char const *name, size_t name_len);
// Unlink a file from the given directory.
// If this is the last reference to an inode, the inode is deleted.
errno_t      vfs_unlink(vfs_file_obj_t *dir, char const *name, size_t name_len);
// Remove a directory if it is empty.
errno_t      vfs_rmdir(vfs_file_obj_t *dir, char const *name, size_t name_len);

// Read all entries from a directory.
errno_dirent_list_t vfs_dir_read(vfs_file_obj_t *dir);
// Read the directory entry with the matching name.
// Returns a new share of the `dentcache_t` returned.
errno_ptr_t         vfs_dir_find_ent(vfs_file_obj_t *dir, char const *name, size_t name_len);

// Stat a file object.
errno_t vfs_stat(vfs_file_obj_t *file, stat_t *stat);

// Unlink a file from the given directory relative to a dir handle.
// If this is the last reference to an inode, the inode is deleted.
// Fails if this is a directory.
errno_t vfs_unlink(vfs_file_obj_t *dir, char const *name, size_t name_len);
// Create a new hard link from one path to another relative to their respective dirs.
// Fails if `old_path` names a directory.
errno_t vfs_link(vfs_file_obj_t *old_obj, vfs_file_obj_t *new_dir, char const *new_name, size_t new_name_len);
// Create a new symbolic link from one path to another, the latter relative to a dir handle.
errno_t vfs_symlink(
    char const     *target_path,
    size_t          target_path_len,
    vfs_file_obj_t *link_dir,
    char const     *link_name,
    size_t          link_name_len
);
// Create a new named FIFO at a path relative to a dir handle.
errno_t vfs_mkfifo(vfs_file_obj_t *dir, char const *name, size_t name_len);
// Make a device special file; only works on certain filesystem types.
errno_t vfs_mkdevfile(vfs_file_obj_t *dir, char const *name, size_t name_len, devfile_t devfile);

// Create a new pipe with one read and one write end.
vfs_pipe_t   vfs_pipe(int flags);
// Open a file or directory for reading and/or writing given parent directory handle.
errno_fobj_t vfs_file_open(vfs_file_obj_t *dir, char const *name, size_t name_len);
// Duplicate a file or directory handle.
void         vfs_file_push_ref(vfs_file_obj_t *orig);
// Close a file opened by `vfs_file_open`.
// Only raises an error if `file` is an invalid file descriptor.
void         vfs_file_pop_ref(vfs_file_obj_t *file);
// Read bytes from a file.
// The entire read succeeds or the entire read fails, never partial read.
errno_t      vfs_file_read(vfs_file_obj_t *file, fileoff_t offset, uint8_t *readbuf, fileoff_t readlen);
// Write bytes to a file.
// If the file is not large enough, it fails.
// The entire write succeeds or the entire write fails, never partial write.
errno_t      vfs_file_write(vfs_file_obj_t *file, fileoff_t offset, uint8_t const *writebuf, fileoff_t writelen);
// Change the length of a file opened by `vfs_file_open`.
errno_t      vfs_file_resize(vfs_file_obj_t *file, fileoff_t new_size);

// Commit all pending writes on a file to disk.
// The filesystem, if it does caching, must always sync everything to disk at once.
errno_t vfs_file_flush(vfs_file_obj_t *file);
// Commit all pending writes to disk.
// The filesystem, if it does caching, must always sync everything to disk at once.
errno_t vfs_flush(vfs_t *vfs);
