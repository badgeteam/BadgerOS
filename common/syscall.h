
// SPDX-License-Identifier: MIT

#pragma once

#ifdef __ASSEMBLER__

// Assembly enum.
#define SYSCALL_DEF(num, enum, name, returns, ...) .equ enum, num
#include "syscall_defs.inc"

#else

#ifdef BADGEROS_KERNEL
#include "filesystem.h"
#include "scheduler/scheduler.h"
#else
typedef int file_t;
typedef int tid_t;

// Open for read-only.
#define OFLAGS_READONLY  0x00000001
// Open for write-only.
#define OFLAGS_WRITEONLY 0x00000002
// Open for read and write.
#define OFLAGS_READWRITE 0x00000003
// Seek to the end on opening.
#define OFLAGS_APPEND    0x00000004
// Truncate on opening.
#define OFLAGS_TRUNCATE  0x00000008
// Create if it doesn't exist on opening.
#define OFLAGS_CREATE    0x00000010
// Error if it exists on opening.
#define OFLAGS_EXCLUSIVE 0x00000020
// Do not inherit to child process.
#define OFLAGS_CLOEXEC   0x00000040
// Open a directory instead of a file.
#define OFLAGS_DIRECTORY 0x00000080

#define MEMFLAGS_R   0x00000001
#define MEMFLAGS_W   0x00000002
#define MEMFLAGS_RW  0x00000003
#define MEMFLAGS_X   0x00000004
#define MEMFLAGS_RX  0x00000005
#define MEMFLAGS_WX  0x00000006
#define MEMFLAGS_RWX 0x00000007
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// C enum.
typedef enum {
#define SYSCALL_DEF(num, enum, name, returns, ...) enum = num,
#include "syscall_defs.inc"
} syscall_t;

// C declarations.
#define SYSCALL_DEF(num, enum, name, returns, ...) returns name(__VA_ARGS__);
#include "syscall_defs.inc"

#endif
