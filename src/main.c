
// SPDX-License-Identifier: MIT

#include "assertions.h"
#include "filesystem.h"
#include "filesystem/vfs_internal.h"
#include "filesystem/vfs_ramfs.h"
#include "gpio.h"
#include "log.h"
#include "malloc.h"
#include "memprotect.h"
#include "port/interrupt.h"
#include "rawprint.h"
#include "scheduler.h"
#include "time.h"

#include <stdint.h>

// Temporary kernel context until threading is implemented.
static kernel_ctx_t kctx;

void debug_func(void *);
#define stack_size 1024
static uint8_t stack0[stack_size] ALIGNED_TO(STACK_ALIGNMENT);

// This is the entrypoint after the stack has been set up and the init functions
// have been run. Main is not allowed to return, so declare it noreturn.
void main() NORETURN;
void main() {
    badge_err_t err;

    // Install interrupt and trap handlers.
    interrupt_init(&kctx);

    // Set up memory protection.
    memprotect_init();

    // Set up timers and watchdogs.
    // This function must run within the first ~1s of power-on time and should be
    // called as early as possible.
    time_init();

    // Set up memory allocator.
    kernel_heap_init();

    // Set up multithreading.
    sched_init(&err);
    assert_always(badge_err_is_ok(&err));

    // A debug thread.
    sched_thread_t *const debug_thread_0 =
        sched_create_kernel_thread(&err, debug_func, NULL, stack0, stack_size, SCHED_PRIO_NORMAL);
    assert_always(badge_err_is_ok(&err));
    sched_set_name(&err, debug_thread_0, "debug0");
    assert_always(badge_err_is_ok(&err));
    sched_resume_thread(&err, debug_thread_0);
    assert_always(badge_err_is_ok(&err));

    // Enter the scheduler
    sched_exec();
    __builtin_unreachable();
}

void debug_func(void *arg) {
    badge_err_t ec;

    // Create RAMFS.
    logk(LOG_DEBUG, "Creating RAMFS at /");
    fs_mount(&ec, FS_TYPE_RAMFS, NULL, "/", 0);
    assert_always(badge_err_is_ok(&ec));

    // Get the VFS handle.
    vfs_t *vfs = &vfs_table[vfs_root_index];

    // Dump raw RAMFS dirents.
    vfs_ramfs_inode_t *inode = &vfs->ramfs.inode_list[vfs->inode_root];
    logk_hexdump(LOG_DEBUG, "raw dirents:", inode->buf, inode->len);

    // Open root directory.
    logk(LOG_DEBUG, "Raw opening root directory");
    vfs_file_shared_t shared;
    vfs_ramfs_root_open(&ec, vfs, &shared);
    assert_always(badge_err_is_ok(&ec));
    shared.refcount = 1;
    vfs_file_handle_t handle;
    handle.offset         = 0;
    handle.write          = false;
    handle.read           = true;
    handle.mutex          = MUTEX_T_INIT_SHARED;
    handle.dir_cache_size = 0;
    handle.dir_cache      = NULL;
    handle.shared         = &shared;
    handle.fileno         = 1;

    // List root directory.
    logk(LOG_DEBUG, "Raw reading directory");
    vfs_ramfs_dir_read(&ec, vfs, &handle);
    assert_always(badge_err_is_ok(&ec));
    logk_hexdump_vaddr(LOG_DEBUG, "dirent cache:", handle.dir_cache, handle.dir_cache_size, 0);



    // Create a file.
    logk(LOG_DEBUG, "Opening a file at /a.txt");
    file_t fd = fs_open(&ec, "/a.txt", OFLAGS_CREATE | OFLAGS_READWRITE);
    assert_always(badge_err_is_ok(&ec));

    // Write some data to it.
    logk(LOG_DEBUG, "Writing data to file");
    fs_write(&ec, fd, "Hi.", 3);
    assert_always(badge_err_is_ok(&ec));

    // Seek to start.
    logk(LOG_DEBUG, "Seeking to 0");
    fs_seek(&ec, fd, 0, SEEK_ABS);
    assert_always(badge_err_is_ok(&ec));

    // Read some data from it.
    logk(LOG_DEBUG, "Reading data from file");
    char      readbuf[4];
    fileoff_t len = fs_read(&ec, fd, readbuf, 3);
    assert_always(badge_err_is_ok(&ec));
    logk_hexdump_vaddr(LOG_DEBUG, "Read data:", readbuf, 3, 0);



    // (void)arg;
    // io_mode(NULL, 19, IO_MODE_OUTPUT);
    // timestamp_us_t t;
    // while (1) {
    //     io_write(NULL, 19, true);
    //     t = time_us() + 500000;
    //     while (time_us() < t) sched_yield();
    //     io_write(NULL, 19, false);
    //     t = time_us() + 500000;
    //     while (time_us() < t) sched_yield();
    // }
}
