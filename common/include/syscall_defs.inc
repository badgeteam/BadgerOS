
// SPDX-License-Identifier: MIT
// clang-format off



/* ==== DEFAULT MACRO EXPANSIONS ==== */

// Normal syscall definition.
#ifndef SYSCALL_DEF
#define SYSCALL_DEF(no, enum, name, returns, ...)
#endif

// Normal syscall definition (returning void).
#ifndef SYSCALL_DEF_V
#define SYSCALL_DEF_V(no, enum, name, ...) SYSCALL_DEF(no, enum, name, void, __VA_ARGS__)
#endif

// Fast syscall definition.
#ifndef SYSCALL_DEF_F
#define SYSCALL_DEF_F(no, enum, name, returns, ...) SYSCALL_DEF(no, enum, name, returns, __VA_ARGS__)
#endif

// Fast syscall definition (returning void).
#ifndef SYSCALL_DEF_FV
#define SYSCALL_DEF_FV(no, enum, name, ...) SYSCALL_DEF_F(no, enum, name, void, __VA_ARGS__)
#endif



/* ==== THREAD SYSCALLS ==== */
// Implemented in process/syscall_impl.c

// Yield to other threads.
SYSCALL_DEF_V(1, SYSCALL_THREAD_YIELD, syscall_thread_yield)

// Sleep for a certain amount of time.
SYSCALL_DEF_V(53, SYSCALL_THREAD_SLEEP, syscall_thread_sleep, int64_t delay)

// Create a new thread.
// Returns thread ID or -errno.
SYSCALL_DEF(2, SYSCALL_THREAD_CREATE, syscall_thread_create, long, void *entry, void *arg, int priority)

// Detach a thread; the thread will be destroyed as soon as it exits.
// Returns 0 or -errno.
SYSCALL_DEF(3, SYSCALL_THREAD_DETACH, syscall_thread_detach, int, long thread)

// Wait for a thread to stop and return its exit code.
// Returns the exit code of that thread or -errno.
SYSCALL_DEF(4, SYSCALL_THREAD_JOIN, syscall_thread_join, int, long thread)

// Exit the current thread; exit code can be read unless destroyed or detached.
SYSCALL_DEF_V(5, SYSCALL_THREAD_EXIT, syscall_thread_exit, int code)



/* ==== PROCESS MANAGEMENT SYSCALLS ==== */
// Implemented in process/syscall_impl.c

// Exit the process; exit code can be read by parent process.
SYSCALL_DEF_V(8, SYSCALL_PROC_EXIT, syscall_proc_exit, int code)

// Get the command-line arguments (i.e. argc+argv) of the current process.
// If memory is large enough, a NULL-terminated argv array of C-string pointers and their data is stored in `memory`.
// The function returns how many bytes would be needed to store the structure.
// If the memory was not large enough, it it not modified.
SYSCALL_DEF(9, SYSCALL_PROC_GETARGS, syscall_proc_getargs, size_t, size_t cap, void *memory)

// Create a "pre-start" child process that inherits this process' file descriptors and environment variables.
// The child process is created in a halted state and it is killed if the parent exits before the child is resumed.
// Returns process ID of new child on success, or a (negative) errno on failure.
SYSCALL_DEF(10, SYSCALL_PROC_PCREATE, syscall_proc_pcreate, int, char const *binary, int argc, char const *const *argv)

// Destroy a "pre-start" child process.
// Usually used in case of errors.
SYSCALL_DEF(11, SYSCALL_PROC_PDESTROY, syscall_proc_pdestroy, bool, int child)

// Starts a "pre-start" child process, thereby converting it into a running child process.
SYSCALL_DEF(12, SYSCALL_PROC_PSTART, syscall_proc_pstart, bool, int child)

// Set the signal handler for a specific signal number.
SYSCALL_DEF(13, SYSCALL_PROC_SIGHANDLER, syscall_proc_sighandler, void *, int signum, void *newhandler)

// Return from a signal handler.
SYSCALL_DEF_V(14, SYSCALL_PROC_SIGRET, syscall_proc_sigret)

// Get child process status update.
SYSCALL_DEF(15, SYSCALL_PROC_WAITPID, syscall_proc_waitpid, int, int pid, int *wstatus, int options)


/* ==== FILESYSTEM SYSCALLS ==== */
// Implemented in filesystem/syscall_impl.c

// Open a file, optionally relative to a directory.
// If `at` is -1, `path` is relative to the working directory.
// Returns -errno on error, file descriptor number on success.
SYSCALL_DEF(16, SYSCALL_FS_OPEN, syscall_fs_open, long, long at, char const *path, size_t path_len, int oflags)

// Flush and close a file.
SYSCALL_DEF(17, SYSCALL_FS_CLOSE, syscall_fs_close, int, long fd)

// Read bytes from a file.
// Returns 0 on EOF, -errno on error, read count on success.
SYSCALL_DEF(18, SYSCALL_FS_READ, syscall_fs_read, long, long fd, void *read_buf, long read_len)

// Write bytes to a file.
// Returns -errno on error, write count on success.
SYSCALL_DEF(19, SYSCALL_FS_WRITE, syscall_fs_write, long, long fd, void const *write_buf, long write_len)

// Read directory entries from a directory handle.
// See `dirent_t` for the format.
// Returns -errno on error, read count on success.
SYSCALL_DEF(20, SYSCALL_FS_GETDENTS, syscall_fs_getdents, long, long fd, void *read_buf, long read_len)

// // Rename and/or move a file to another path, optionally relative to one or two directories.
// SYSCALL_DEF_V(21, SYSCALL_FS_RENAME, syscall_fs_rename)

// Get file status given file handler or path, optionally following the final symlink.
// If `path` is specified, it is interpreted as relative to the working directory.
// If both `path` and `fd` are specified, `path` is relative to the directory that `fd` describes.
// If only `fd` is specified, the inode referenced by `fd` is stat'ed.
// If `follow_link` is false, the last symlink in the path is not followed.
SYSCALL_DEF(22, SYSCALL_FS_STAT, syscall_fs_stat, int, long fd, char const *path, size_t path_len, bool follow_link, stat_t *stat_out)

// Create a new directory.
// If `at` is -1, `path` is relative to the working directory.
// Returns -errno on error, file 0 on success.
SYSCALL_DEF(47, SYSCALL_FS_MKDIR, syscall_fs_mkdir, int, long at, char const *path, size_t path_len)

// Delete a directory if it is empty.
// If `at` is -1, `path` is relative to the working directory.
// Returns -errno on error, file 0 on success.
SYSCALL_DEF(48, SYSCALL_FS_RMDIR, syscall_fs_rmdir, int, long at, char const *path, size_t path_len)

// Create a new link to an existing inode.
// If `*_at` is -1, the respective `*_path` is relative to the working directory.
// Returns -errno on error, file 0 on success.
SYSCALL_DEF(49, SYSCALL_FS_LINK, syscall_fs_link, int, long old_at, char const *old_path, size_t old_path_len, long new_at, char const *new_path, size_t new_path_len)

// Remove a link to an inode. If it is the last link, the file is deleted.
// If `at` is -1, `path` is relative to the working directory.
// Returns -errno on error, file 0 on success.
SYSCALL_DEF(50, SYSCALL_FS_UNLINK, syscall_fs_unlink, int, long at, char const *path, size_t path_len)

// Create a new FIFO / named pipe.
// If `at` is -1, `path` is relative to the working directory.
// Returns -errno on error, file 0 on success.
SYSCALL_DEF(51, SYSCALL_FS_MKFIFO, syscall_fs_mkfifo, int, long at, char const *path, size_t path_len)

// Create a new pipe.
// `fds[0]` will be written with the pointer to the read end, `fds[1]` the write end.
// Returns -errno on error, file 0 on success.
SYSCALL_DEF(52, SYSCALL_FS_PIPE, syscall_fs_pipe, int, long fds[2], int flags)



/* ==== MEMORY MANAGEMENT SYSCALLS ==== */
// Implemented in process/syscall_impl.c

// Map a new range of memory at an arbitrary virtual address.
// This may round up to a multiple of the page size.
// Alignment may be less than `align` if the kernel doesn't support it.
SYSCALL_DEF(23, SYSCALL_MEM_MAP, syscall_mem_map, void *, void *address, size_t size, size_t align, int flags)

// Unmap a range of memory previously allocated with `SYSCALL_MEM_MAP`.
// Returns whether a range of memory was unmapped.
SYSCALL_DEF(25, SYSCALL_MEM_UNMAP, syscall_mem_unmap, bool, void *address, size_t size)



/* ==== SYSTEM MANAGEMENT SYSCALLS ==== */
// Implemented in main.c

// Start the shutdown process.
SYSCALL_DEF_V(45, SYSCALL_SYS_SHUTDOWN, syscall_sys_shutdown, bool is_reboot)



/* ==== TEMPORARY SYSCALLS ==== */
// Implemented in process/syscall_impl.c

// TEMPORARY: Write one or more characters to the log.
SYSCALL_DEF_V(46, SYSCALL_TEMP_WRITE, syscall_temp_write, char const *message, size_t length)



#undef SYSCALL_DEF
#undef SYSCALL_DEF_V
#undef SYSCALL_DEF_F
#undef SYSCALL_DEF_FV
