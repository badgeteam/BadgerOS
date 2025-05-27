
// SPDX-License-Identifier: MIT

#include "cpu/mmu.h"
#include "filesystem.h"
#include "process/internal.h"
#include "syscall.h"
#include "syscall_util.h"
#include "usercopy.h"

#define GET_REAL_AT(u_at, at)                                                                                          \
    file_t at = -1;                                                                                                    \
    if (u_at != -1) {                                                                                                  \
        at = proc_find_fd_raw(proc_current(), u_at);                                                                   \
        if (at < 0) {                                                                                                  \
            return at;                                                                                                 \
        }                                                                                                              \
    }

#define GET_REAL_FD(u_fd, fd)                                                                                          \
    file_t fd = proc_find_fd_raw(proc_current(), u_fd);                                                                \
    if (fd < 0) {                                                                                                      \
        return fd;                                                                                                     \
    }

// Open a file, optionally relative to a directory.
// If `at` is -1, `path` is relative to the working directory.
// Returns -errno on error, file descriptor number on success.
long syscall_fs_open(long u_at, char const *path, size_t path_len, int oflags) {
    sysutil_memassert_r(path, path_len);
    GET_REAL_AT(u_at, at);
    char pathbuf[FILESYSTEM_PATH_MAX];
    copy_from_user_raw(proc_current(), pathbuf, (size_t)path, path_len);
    file_t fd   = fs_open(at, pathbuf, path_len, oflags);
    long   u_fd = proc_add_fd_raw(proc_current(), fd);
    if (u_fd < 0) {
        fs_close(fd);
    }
    return u_fd;
}

// Flush and close a file.
int syscall_fs_close(long u_fd) {
    GET_REAL_FD(u_fd, fd);
    return fs_close(fd);
}

// Read bytes from a file.
// Returns 0 on EOF, -errno on error, read count on success.
long syscall_fs_read(long u_fd, void *read_buf, long read_len) {
    sysutil_memassert_rw(read_buf, read_len);
    GET_REAL_FD(u_fd, fd);
#if !CONFIG_NOMMU
    mmu_enable_sum();
#endif
    long res = fs_read(fd, read_buf, read_len);
#if !CONFIG_NOMMU
    mmu_disable_sum();
#endif
    return res;
}

// Write bytes to a file.
// Returns -errno on error, write count on success.
long syscall_fs_write(long u_fd, void const *write_buf, long write_len) {
    sysutil_memassert_r(write_buf, write_len);
    GET_REAL_FD(u_fd, fd);
#if !CONFIG_NOMMU
    mmu_enable_sum();
#endif
    long res = fs_write(fd, write_buf, write_len);
#if !CONFIG_NOMMU
    mmu_disable_sum();
#endif
    return res;
}

// Read directory entries from a directory handle.
// See `dirent_t` for the format.
// Returns -errno on error, read count on success.
long syscall_fs_getdents(long u_fd, void *read_buf, long read_len) {
    sysutil_memassert_rw(read_buf, read_len);
    GET_REAL_FD(u_fd, fd);
    errno_dirent_list_t res = fs_dir_read(fd);
    if (res.errno < 0) {
        return res.errno;
    }
    long actual_len = read_len < res.list.size ? read_len : res.list.size;
    copy_to_user_raw(proc_current(), (size_t)read_buf, res.list.mem, actual_len);
    return actual_len;
}

// // Rename and/or move a file to another path, optionally relative to one or two directories.
// SYSCALL_DEF_V(21, SYSCALL_FS_RENAME, syscall_fs_rename)

// Get file status given file handler or path, optionally following the final symlink.
// If `path` is specified, it is interpreted as relative to the working directory.
// If both `path` and `fd` are specified, `path` is relative to the directory that `fd` describes.
// If only `fd` is specified, the inode referenced by `fd` is stat'ed.
// If `follow_link` is false, the last symlink in the path is not followed.
int syscall_fs_stat(long u_fd, char const *path, size_t path_len, bool follow_link, stat_t *stat_out) {
    sysutil_memassert_r(path, path_len);
    sysutil_memassert_rw(stat_out, sizeof(stat_t));
    GET_REAL_FD(u_fd, fd);
    char pathbuf[FILESYSTEM_PATH_MAX];
    copy_from_user_raw(proc_current(), pathbuf, (size_t)path, path_len);
    stat_t  statbuf;
    errno_t res = fs_stat(fd, pathbuf, path_len, follow_link, &statbuf);
    copy_to_user_raw(proc_current(), (size_t)stat_out, &statbuf, sizeof(stat_t));
    return res;
}

// Create a new directory.
// If `at` is -1, `path` is relative to the working directory.
// Returns -errno on error, file 0 on success.
int syscall_fs_mkdir(long u_at, char const *path, size_t path_len) {
    sysutil_memassert_r(path, path_len);
    GET_REAL_AT(u_at, at);
    char pathbuf[FILESYSTEM_PATH_MAX];
    copy_from_user_raw(proc_current(), pathbuf, (size_t)path, path_len);
    return fs_mkdir(at, pathbuf, path_len);
}

// Delete a directory if it is empty.
// If `at` is -1, `path` is relative to the working directory.
// Returns -errno on error, file 0 on success.
int syscall_fs_rmdir(long u_at, char const *path, size_t path_len) {
    sysutil_memassert_r(path, path_len);
    GET_REAL_AT(u_at, at);
    char pathbuf[FILESYSTEM_PATH_MAX];
    copy_from_user_raw(proc_current(), pathbuf, (size_t)path, path_len);
    return fs_rmdir(at, pathbuf, path_len);
}

// Create a new link to an existing inode.
// If `*_at` is -1, the respective `*_path` is relative to the working directory.
// Returns -errno on error, file 0 on success.
int syscall_fs_link(
    long u_old_at, char const *old_path, size_t old_path_len, long u_new_at, char const *new_path, size_t new_path_len
) {
    sysutil_memassert_r(old_path, old_path_len);
    sysutil_memassert_r(new_path, new_path_len);
    GET_REAL_AT(u_old_at, old_at);
    GET_REAL_AT(u_new_at, new_at);
    char new_pathbuf[FILESYSTEM_PATH_MAX];
    copy_from_user_raw(proc_current(), new_pathbuf, (size_t)new_path, new_path_len);
    char old_pathbuf[FILESYSTEM_PATH_MAX];
    copy_from_user_raw(proc_current(), old_pathbuf, (size_t)old_path, old_path_len);
    return fs_link(old_at, old_pathbuf, old_path_len, new_at, new_pathbuf, new_path_len);
}

// Remove a link to an inode. If it is the last link, the file is deleted.
// If `at` is -1, `path` is relative to the working directory.
// Returns -errno on error, file 0 on success.
int syscall_fs_unlink(long u_at, char const *path, size_t path_len) {
    sysutil_memassert_r(path, path_len);
    GET_REAL_AT(u_at, at);
    char pathbuf[FILESYSTEM_PATH_MAX];
    copy_from_user_raw(proc_current(), pathbuf, (size_t)path, path_len);
    return fs_unlink(at, pathbuf, path_len);
}

// Create a new FIFO / named pipe.
// If `at` is -1, `path` is relative to the working directory.
// Returns -errno on error, file 0 on success.
int syscall_fs_mkfifo(long u_at, char const *path, size_t path_len) {
    sysutil_memassert_r(path, path_len);
    GET_REAL_AT(u_at, at);
    char pathbuf[FILESYSTEM_PATH_MAX];
    copy_from_user_raw(proc_current(), pathbuf, (size_t)path, path_len);
    return fs_mkfifo(at, pathbuf, path_len);
}

// Create a new pipe.
// `fds[0]` will be written with the pointer to the read end, `fds[1]` the write end.
// Returns -errno on error, file 0 on success.
int syscall_fs_pipe(long u_fds_out[2], int flags) {
    sysutil_memassert_rw(u_fds_out, 2 * sizeof(long));
    fs_pipe_t res = fs_pipe(flags);
    if (res.errno < 0) {
        return res.errno;
    }

    long u_reader = proc_add_fd_raw(proc_current(), res.reader);
    if (u_reader < 0) {
        fs_close(res.reader);
        fs_close(res.writer);
        return u_reader;
    }

    long u_writer = proc_add_fd_raw(proc_current(), res.writer);
    if (u_writer < 0) {
        proc_remove_fd_raw(proc_current(), u_reader);
        fs_close(res.reader);
        fs_close(res.writer);
        return u_writer;
    }

#if !CONFIG_NOMMU
    mmu_enable_sum();
#endif
    u_fds_out[0] = u_reader;
    u_fds_out[1] = u_writer;
#if !CONFIG_NOMMU
    mmu_disable_sum();
#endif

    return 0;
}
