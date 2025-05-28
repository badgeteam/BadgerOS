
// SPDX-License-Identifier: MIT

#include "process/internal.h"
#include "scheduler/scheduler.h"



// Implementation of thread yield system call.
void syscall_thread_yield() {
    thread_yield();
}

// Implementation of usleep system call.
void syscall_thread_sleep(timestamp_us_t delay) {
    thread_sleep(delay);
}

// Create a new thread.
// Returns thread ID or -errno.
long syscall_thread_create(void *entry, void *arg, int priority) {
    return proc_create_thread_raw(proc_current(), (size_t)entry, (size_t)arg, priority);
}

// Detach a thread; the thread will be destroyed as soon as it exits.
// Returns 0 or -errno.
int syscall_thread_detach(long u_tid) {
    return -ENOTSUP;
}

// Wait for a thread to stop and return its exit code.
// Returns the exit code of that thread or -errno.
int syscall_thread_join(long u_tid) {
    return -ENOTSUP;
}

// Exit the current thread; exit code can be read unless destroyed or detached.
void syscall_thread_exit(int code) {
    thread_exit(code);
}
