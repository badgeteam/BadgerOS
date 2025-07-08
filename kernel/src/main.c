
// SPDX-License-Identifier: MIT

#include "assertions.h"
#include "badge_strings.h"
#include "bootp.h"
#include "cpulocal.h"
#include "device/class/block.h"
#include "device/dev_class.h"
#include "device/device.h"
#include "errno.h"
#include "filesystem.h"
#include "housekeeping.h"
#include "interrupt.h"
#include "isr_ctx.h"
#include "kmodule.h"
#include "log.h"
#include "malloc.h"
#include "memprotect.h"
#include "panic.h"
#include "port/port.h"
#include "process/process.h"
#include "radixtree.h"
#include "rawprint.h"
#include "scheduler/scheduler.h"
#include "set.h"
#include "time.h"

#include <stdatomic.h>



// When set, a shutdown is initiated.
// 0: Do nothing.
// 1: Shut down (default).
// 2: Reboot.
atomic_int              kernel_shutdown_mode;
// Thread ID of kernel lifetime thread.
static tid_t            klifetime_tid;
// Set of threads that will be joined before kernel init comletes.
static set_t            kinit_join_threads = PTR_SET_EMPTY;
// Built-in kernel modules.
extern kmodule_t const *start_kmodules[] asm("__start_kmodules");
extern kmodule_t const *stop_kmodules[] asm("__stop_kmodules");

void        init_ramfs();
static void kernel_init();
static void userland_init();
static void userland_shutdown();
static void kernel_shutdown();
static void kernel_lifetime_func();



// CPU-local data of booting CPU.
static cpulocal_t bsp_cpulocal = {0};

// After control handover, the booting CPU core starts here and other cores wait.
// This sets up the basics of everything needed by the other systems of the kernel.
// When finished, the booting CPU will perform kernel initialization.
void basic_runtime_init() {
    isr_ctx_t tmp_ctx = {0};
    tmp_ctx.flags     = ISR_CTX_FLAG_KERNEL;
    tmp_ctx.cpulocal  = &bsp_cpulocal;

    // ISR initialization.
    irq_init(&tmp_ctx);
    // Early memory protection initialization.
    memprotect_early_init();
    // Early platform initialization.
    bootp_early_init();

    // Announce that we're alive.
    logk_from_isr(LOG_INFO, "BadgerOS " CONFIGSTR_CPU " starting...");

    // Kernel memory allocator initialization.
    kernel_heap_init();

    // Post-heap memory protection initialization.
    memprotect_postheap_init();

    // Global scheduler initialization.
    sched_init();
    sched_init_cpu(0);

    // Housekeeping thread initialization.
    hk_init();
    // Add the remainder of the kernel lifetime as a new thread.
    klifetime_tid = thread_new_kernel("main", (void *)kernel_lifetime_func, NULL, SCHED_PRIO_NORMAL);
    assert_always(klifetime_tid > 0);
    assert_always(thread_resume(klifetime_tid) >= 0);

    // Start the scheduler and enter the next phase in the kernel's lifetime.
    sched_exec();
    assert_unreachable();
}


// Manages the kernel's lifetime after basic runtime initialization.
static void kernel_lifetime_func() {
    // Initialize the built-in kernel modules.
    for (kmodule_t const **cur = start_kmodules; cur != stop_kmodules; cur++) {
        logkf(LOG_INFO, "Init built-in module '%{cs}'", (**cur).name);
        (**cur).init();
    }

    // Start the kernel services.
    kernel_init();
    // Start other CPUs.
    sched_start_altcpus();
    // After secondary CPUs are started, any potential reclaiming of bootloader memory is possible.
    bootp_reclaim_mem();
    // Start userland.
    userland_init();

    // The boot process is now complete, this thread will wait until a shutdown is issued.
    int shutdown_mode;
    do {
        thread_yield();
        shutdown_mode = atomic_load(&kernel_shutdown_mode);
    } while (shutdown_mode == 0);

    // Shut down the userland.
    userland_shutdown();
    // Tie up loose ends.
    kernel_shutdown();
    // Power off.
    if (kernel_shutdown_mode == 2) {
        logkf(LOG_INFO, "Restarting");
        port_poweroff(true);
    } else {
        logkf(LOG_INFO, "Powering off");
        port_poweroff(false);
    }
}

// Shutdown system call implementation.
void syscall_sys_shutdown(bool is_reboot) {
    logk(LOG_INFO, is_reboot ? "Reboot requested" : "Shutdown requested");
    atomic_store(&kernel_shutdown_mode, 1 + is_reboot);
}



void dummy_print(int indent, void *ptr) {
    rawprint(ptr);
    rawputc('\n');
}

static void dumpdir(file_t at, char const *path, int indent) {
    if (cstr_equals(path, ".") || cstr_equals(path, ".."))
        return;
    file_t fd = fs_dir_open(at, path, cstr_length(path), 0);
    for (int i = 0; i < indent; i++) rawprint("  ");
    rawprint(path);
    rawputc('\n');
    if (!fd)
        return;
    errno_dirent_list_t list = fs_dir_read(fd);
    if (list.errno >= 0) {
        dirent_t *ent = list.list.mem;
        for (size_t i = 0; i < list.list.ent_count; i++) {
            dumpdir(at, ent->name, indent + 1);
            ent = (dirent_t *)((size_t)ent + ent->record_len);
        }
        free(list.list.mem);
    }
    fs_dir_close(fd);
}

// After basic runtime initialization, the booting CPU core continues here.
// This finishes the initialization of all kernel systems, resources and services.
// When finished, the non-booting CPUs will be started (method and entrypoints to be determined).
static void kernel_init() {
    // Memory protection initialization.
    memprotect_init();
    // Full hardware initialization.
    bootp_full_init();

    // Wait for some final threads before mounting root FS.
    set_foreach(void, tid, &kinit_join_threads) {
        thread_join((tid_t)(size_t)tid);
    }
    set_clear(&kinit_join_threads);

    dev_filter_t filter = {
        .match_class = true,
        .class       = DEV_CLASS_BLOCK,
    };
    set_t devs = device_get_filtered(&filter);
    set_foreach(device_t, dev, &devs) {
        device_block_t *blkdev = (void *)dev;
        rtree_dump(&blkdev->cache, NULL);

        logk(LOG_DEBUG, "Doing a read of the block device");
        uint8_t buf[512];
        errno_t res = device_block_read_bytes(blkdev, 0, 512, buf);
        if (res < 0) {
            logkf(LOG_ERROR, "Failed to read: %{d} (%{cs})", -res, errno_get_name(-res));
        }
        logk_hexdump_vaddr(LOG_DEBUG, "First block:", buf, 512, 0);
        rtree_dump(&blkdev->cache, NULL);

        logk(LOG_DEBUG, "Doing a write of the block device");
        res = device_block_write_bytes(blkdev, 9, 36, "This is some destructive write data.");
        if (res < 0) {
            logkf(LOG_ERROR, "Failed to write: %{d} (%{cs})", -res, errno_get_name(-res));
        }
        rtree_dump(&blkdev->cache, NULL);

        logk(LOG_DEBUG, "Doing a sync of the block device");
        res = device_block_sync_all(blkdev, false);
        if (res < 0) {
            logkf(LOG_ERROR, "Failed to sync: %{d} (%{cs})", -res, errno_get_name(-res));
        }
        rtree_dump(&blkdev->cache, NULL);

        logk(LOG_DEBUG, "Done!");
        device_pop_ref(dev);
    }
    set_clear(&devs);

    // Temporary filesystem image.
    errno_t res;
    res = fs_mount("ramfs", NULL, FILE_NONE, "/", 1, 0);
    assert_always(res >= 0);
    res = fs_mkdir(FILE_NONE, "/dev", 4);
    assert_always(res >= 0);
    res = fs_mount("devtmpfs", NULL, FILE_NONE, "/dev", 4, 0);
    assert_always(res >= 0);
    init_ramfs();

    dumpdir(-1, "/", 0);
}



// After kernel initialization, the booting CPU core continues here.
// This starts up the `init` process while other CPU cores wait for processes to be scheduled for them.
// When finished, this function returns and the thread should wait for a shutdown event.
static void userland_init() {
    logk(LOG_INFO, "Kernel initialized");
    logk(LOG_INFO, "Starting init process");

    char const *initbin = "/sbin/init";
    pid_t       pid     = proc_create(-1, "/sbin/init", 1, &initbin);
    assert_always(pid == 1);
    assert_always(proc_start(pid) >= 0);
}



// When a shutdown event begins, exactly one CPU core runs this entire function.
// This signals all processes to exit (or be killed if they wait too long) and shuts down other CPU cores.
// When finished, the CPU continues to shut down the kernel.
static void userland_shutdown() {
    if (proc_has_noninit()) {
        // Warn all processes of the imminent doom.
        logk(LOG_INFO, "Sending SIGHUP to running processes");
        proc_signal_all(SIGHUP);
        // Wait for one second to give them time.
        timestamp_us_t lim = time_us() + 1000000;
        while (time_us() < lim && proc_has_noninit()) thread_yield();

        if (proc_has_noninit()) {
            // Forcibly terminate all processes.
            logk(LOG_INFO, "Sending SIGKILL to running processes");
            proc_signal_all(SIGKILL);
        }
    }

    // Tell init it's now time to stop.
    logk(LOG_INFO, "Sending SIGHUP to init");
    proc_raise_signal(1, SIGHUP);
    // Wait for a couple seconds to give it time.
    timestamp_us_t lim = time_us() + 5000000;
    while (time_us() < lim) {
        errno64_t flags = proc_getflags(1);
        if (flags == -ENOENT || (flags > 0 && !(flags & PROC_RUNNING))) {
            // When the init process stops, userland has successfully been shut down.
            return;
        }
        thread_yield();
    }

    // If init didn't stop by this point we're probably out of luck.
    logk(LOG_FATAL, "Init process did not stop at shutdown");
    panic_abort();
}



// When the userspace has been shut down, the CPU continues here.
// This will synchronize all filesystems and clean up any other resources not needed to finish hardware shutdown.
// When finished, the CPU continues to the platform-specific hardware shutdown / reboot handler.
static void kernel_shutdown() {
    // TODO: Filesystems flush.
}



// Add a thread to the list to block on for kernel init.
void klifetime_join_for_kinit(tid_t tid) {
    assert_dev_drop(sched_current_tid() == klifetime_tid);
    set_add(&kinit_join_threads, (void *)(size_t)tid);
}
