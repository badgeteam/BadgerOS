
// SPDX-License-Identifier: MIT

#include "filesystem.h"
#include "process/internal.h"
#include "syscall.h"
#include "syscall_util.h"
#include "usercopy.h"

#if !CONFIG_NOMMU
#include "cpu/mmu.h"
#endif

#define PATHBUF(path, path_copy, ...)                                                                                  \
    if (path##_len > PATH_MAX) {                                                                                       \
        __VA_ARGS__;                                                                                                   \
        return -ENAMETOOLONG;                                                                                          \
    }                                                                                                                  \
    char path_copy[PATH_MAX];                                                                                          \
    sigsegv_assert(copy_from_user_raw(proc, path_copy, (size_t)path, path##_len), (size_t)path);

#define GET_FD(u_fd, k_fd, ...)                                                                                        \
    file_t k_fd = FILE_NONE;                                                                                           \
    if (u_fd != -1) {                                                                                                  \
        k_fd = proc_find_fd_raw(proc, u_fd);                                                                           \
        if (!k_fd.metadata) {                                                                                          \
            __VA_ARGS__;                                                                                               \
            return -EBADF;                                                                                             \
        }                                                                                                              \
    }


// Open a file, optionally relative to a directory.
// If `at` is -1, `path` is relative to the working directory.
// Returns -errno on error, file descriptor number on success.
long syscall_fs_open(long u_at, char const *path, size_t path_len, int oflags) {
    process_t *proc = proc_current();
    PATHBUF(path, path_copy);
    GET_FD(u_at, k_at);
    file_t res  = RETURN_ON_ERRNO_FILE(fs_open(k_at, path_copy, path_len, oflags), fs_file_drop(k_at));
    long   u_fd = proc_add_fd_raw(proc, res);
    if (u_fd < 0) {
        fs_file_drop(res);
    }
    return u_fd;
}

// Flush and close a file.
int syscall_fs_close(long u_fd) {
    return proc_remove_fd_raw(proc_current(), u_fd);
}

// Read bytes from a file.
// Returns 0 on EOF, -errno on error, read count on success.
long syscall_fs_read(long u_fd, void *rdata, long rdata_len) {
    if (rdata_len < 0) {
        return -EINVAL;
    }
    process_t *proc = proc_current();
    sysutil_memassert_rw(rdata, rdata_len);
    GET_FD(u_fd, k_fd);

#if !CONFIG_NOMMU
    mmu_enable_sum();
#endif
    errno64_t res = fs_read(k_fd, rdata, rdata_len);
#if !CONFIG_NOMMU
    mmu_disable_sum();
#endif

    fs_file_drop(k_fd);
    return res;
}

// Write bytes to a file.
// Returns -errno on error, write count on success.
long syscall_fs_write(long u_fd, void const *wdata, long wdata_len) {
    if (wdata_len < 0) {
        return -EINVAL;
    }
    process_t *proc = proc_current();
    sysutil_memassert_r(wdata, wdata_len);
    GET_FD(u_fd, k_fd);

#if !CONFIG_NOMMU
    mmu_enable_sum();
#endif
    errno64_t res = fs_write(k_fd, wdata, wdata_len);
#if !CONFIG_NOMMU
    mmu_disable_sum();
#endif

    fs_file_drop(k_fd);
    return res;
}

// Read directory entries from a directory handle.
// See `dirent_t` for the format.
// Returns -errno on error, read count on success.
long syscall_fs_getdents(long u_fd, void *read_buf, long read_len) {
    return -ENOSYS;
}

// Rename and/or move a file to another path, optionally relative to one or two directories.
// If `*_at` is -1, the respective `*_path` is relative to the working directory.
// Returns -errno on error, 0 on success.
int syscall_fs_rename(syscall_fs_rename_args_t *args) {
    process_t               *proc = proc_current();
    syscall_fs_rename_args_t copy;
    sigsegv_assert(copy_from_user_raw(proc, &copy, (size_t)args, sizeof(copy)), (size_t)args);
    PATHBUF(copy.old_path, old_path_copy);
    PATHBUF(copy.new_path, new_path_copy);
    GET_FD(copy.old_at, k_old_at);
    GET_FD(copy.new_at, k_new_at, fs_file_drop(k_old_at));

    errno_t res =
        fs_rename(k_old_at, old_path_copy, copy.old_path_len, k_new_at, new_path_copy, copy.new_path_len, copy.flags);

    fs_file_drop(k_old_at);
    fs_file_drop(k_new_at);
    return -ENOSYS;
}

// Helper for `syscall_fs_stat` where only `u_fd` is given.
static int _syscall_fs_stat_fdonly(long u_fd, stat_t *stat_out) {
    process_t *proc = proc_current();
    GET_FD(u_fd, k_fd);

#if !CONFIG_NOMMU
    mmu_enable_sum();
#endif
    errno_t res = fs_stat(k_fd, stat_out);
#if !CONFIG_NOMMU
    mmu_disable_sum();
#endif

    fs_file_drop(k_fd);
    return res;
}

// Get file status given file handler or path, optionally following the final symlink.
// If `path` is specified, it is interpreted as relative to the working directory.
// If both `path` and `fd` are specified, `path` is relative to the directory that `fd` describes.
// If only `fd` is specified, the inode referenced by `fd` is stat'ed.
// If `follow_link` is false, the last symlink in the path is not followed.
int syscall_fs_stat(long u_fd, char const *path, size_t path_len, bool follow_link, stat_t *stat_out) {
    sysutil_memassert_rw(stat_out, sizeof(stat_t));
    if (!path_len) {
        return _syscall_fs_stat_fdonly(u_fd, stat_out);
    }

    process_t *proc = proc_current();
    PATHBUF(path, path_copy);
    GET_FD(u_fd, k_fd);

    file_t stat_fd = RETURN_ON_ERRNO_FILE(
        fs_open(k_fd, path_copy, path_len, follow_link ? FS_O_READ_ONLY : FS_O_READ_ONLY | FS_O_NOFOLLOW),
        fs_file_drop(k_fd);
    );
#if !CONFIG_NOMMU
    mmu_enable_sum();
#endif
    errno_t res = fs_stat(stat_fd, stat_out);
#if !CONFIG_NOMMU
    mmu_disable_sum();
#endif

    fs_file_drop(k_fd);
    fs_file_drop(stat_fd);
    return -ENOSYS;
}

// Helper for syscalls which call `fs_make_file`.
static int _syscall_fs_make_file(long u_at, char const *path, size_t path_len, make_file_spec_t spec) {
    process_t *proc = proc_current();
    PATHBUF(path, path_copy);
    GET_FD(u_at, k_at);
    errno_t res = fs_make_file(k_at, path_copy, path_len, spec);
    fs_file_drop(k_at);
    return res;
}

// Helper for syscalls which call `fs_unlink`.
static int _syscall_fs_unlink(long u_at, char const *path, size_t path_len, bool is_rmdir) {
    process_t *proc = proc_current();
    PATHBUF(path, path_copy);
    GET_FD(u_at, k_at);
    errno_t res = fs_unlink(k_at, path_copy, path_len, is_rmdir);
    fs_file_drop(k_at);
    return res;
}

// Create a new directory.
// If `at` is -1, `path` is relative to the working directory.
// Returns -errno on error, 0 on success.
int syscall_fs_mkdir(long u_at, char const *path, size_t path_len) {
    return _syscall_fs_make_file(u_at, path, path_len, (make_file_spec_t){.type = NODE_TYPE_DIRECTORY});
}

// Delete a directory if it is empty.
// If `at` is -1, `path` is relative to the working directory.
// Returns -errno on error, 0 on success.
int syscall_fs_rmdir(long u_at, char const *path, size_t path_len) {
    return _syscall_fs_unlink(u_at, path, path_len, true);
}

// Create a new link to an existing inode.
// If `*_at` is -1, the respective `*_path` is relative to the working directory.
// Returns -errno on error, 0 on success.
int syscall_fs_link(syscall_fs_rename_args_t *args) {
    process_t               *proc = proc_current();
    syscall_fs_rename_args_t copy;
    sigsegv_assert(copy_from_user_raw(proc, &copy, (size_t)args, sizeof(copy)), (size_t)args);
    PATHBUF(copy.old_path, old_path_copy);
    PATHBUF(copy.new_path, new_path_copy);
    GET_FD(copy.old_at, k_old_at);
    GET_FD(copy.new_at, k_new_at, fs_file_drop(k_old_at));

    errno_t res =
        fs_link(k_old_at, old_path_copy, copy.old_path_len, k_new_at, new_path_copy, copy.new_path_len, copy.flags);

    fs_file_drop(k_old_at);
    fs_file_drop(k_new_at);
    return -ENOSYS;
}

// Remove a link to an inode. If it is the last link, the file is deleted.
// If `at` is -1, `path` is relative to the working directory.
// Returns -errno on error, 0 on success.
int syscall_fs_unlink(long u_at, char const *path, size_t path_len) {
    return _syscall_fs_unlink(u_at, path, path_len, false);
}

// Create a new FIFO / named pipe.
// If `at` is -1, `path` is relative to the working directory.
// Returns -errno on error, 0 on success.
int syscall_fs_mkfifo(long u_at, char const *path, size_t path_len) {
    return _syscall_fs_make_file(u_at, path, path_len, (make_file_spec_t){.type = NODE_TYPE_FIFO});
}

// Create a new pipe.
// `fds[0]` will be written with the pointer to the read end, `fds[1]` the write end.
// Returns -errno on error, 0 on success.
int syscall_fs_pipe(long u_fds_out[2], int flags) {
    sysutil_memassert_rw(u_fds_out, sizeof(long) * 2);
    process_t *proc = proc_current();
    fs_pipe_t  pipe = fs_pipe(flags);
    RETURN_ON_ERRNO(pipe.errno);

    long read_fd = proc_add_fd_raw(proc, pipe.read_end);
    if (read_fd < 0) {
        fs_file_drop(pipe.read_end);
        fs_file_drop(pipe.write_end);
        return read_fd;
    }

    long write_fd = proc_add_fd_raw(proc, pipe.write_end);
    if (write_fd < 0) {
        proc_remove_fd_raw(proc, read_fd);
        fs_file_drop(pipe.write_end);
        return write_fd;
    }

#if !CONFIG_NOMMU
    mmu_enable_sum();
#endif
    u_fds_out[0] = read_fd;
    u_fds_out[1] = write_fd;
#if !CONFIG_NOMMU
    mmu_disable_sum();
#endif

    return 0;
}
