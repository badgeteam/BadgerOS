
// SPDX-FileCopyrightText: 2025 Julian Scheffers <julian@scheffers.net>
// SPDX-FileType: SOURCE
// SPDX-License-Identifier: MIT

#pragma once

#include "errno.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Note: These definitions are copied from the Rust code, do not change them!

// Kinds of usage for pages of memory.
typedef enum {
    // Unused page.
    Free = 0,
    // Part of a page table.
    PageTable,
    // Contains cached data.
    Cache,
    // Part of a mmap'ed file.
    Mmap,
    // Anonymous user memory.
    UserAnon,
    // Anonymous kernel memory.
    KernelAnon,
    // Dummy entry for unusable page.
    Unusable,
} page_usage_t;

// Page struct is locked for modifications.
#define LOCKED     0x00000001u
// Page may be swapped to disk.
#define SWAPPABLE  0x00000002u
// Bitmask: buddy alloc page order.
#define ORDER_MASK 0x00fc0000u
// Bit exponent: buddy alloc page order.
#define ORDER_EXP  18u
// Bitmask: Page usage.
#define USAGE_MASK 0xff000000u
// Bit exponent: Page usage.
#define USAGE_EXP  24u

// Physical memory page metadata.
typedef struct {
    // Page refcount, typically 1 for kernel pages.
    atomic_uint refcount;
    // Page flags and usage kind.
    atomic_uint flags;
    // TODO: Pointer to structure that exposes where it's mapped in user virtual memory.
    // Kernel virtual mappings need not be tracked because they are not swappable.
} pmm_page_t;
