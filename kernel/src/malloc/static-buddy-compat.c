
// SPDX-FileCopyrightText: 2025 Julian Scheffers <julian@scheffers.net>
// SPDX-FileType: SOURCE
// SPDX-License-Identifier: MIT

// This file is for temporary backwards compatibility with the old static-buddy.c API for page allocations.

#include "log.h"
#include "mem/pmm.h"
#include "mem/vmm.h"
#include "panic.h"
#include "static-buddy.h"
#include "todo.h"

void *buddy_allocate(size_t size, enum block_type type, uint32_t flags) {
    page_usage_t usage;
    switch (type) {
        default: panic_abort();
        case BLOCK_TYPE_USER: usage = PAGE_USAGE_USER_ANON; break;
        case BLOCK_TYPE_PAGE: usage = PAGE_USAGE_KERNEL_ANON; break;
        case BLOCK_TYPE_SLAB: usage = PAGE_USAGE_KERNEL_SLAB; break;
    }
    uint32_t order = 0;
    while ((1llu << order) < size) {
        order++;
    }
    errno_ppn_t page = pmm_page_alloc(order, usage);
    if (page < 0) {
        logkf(LOG_WARN, "Out of memory (allocating block of order %{u32;d})", order);
        return NULL;
    }
    return (void *)(page * CONFIG_PAGE_SIZE + vmm_hhdm_offset);
}

void *buddy_reallocate(void *ptr, size_t size) {
    TODO();
}

void buddy_deallocate(void *ptr) {
    pmm_page_t *page_meta = pmm_page_struct(((size_t)ptr - vmm_hhdm_offset) / CONFIG_PAGE_SIZE);
    pmm_page_free(
        ((size_t)ptr - vmm_hhdm_offset) / CONFIG_PAGE_SIZE,
        (page_meta->flags & PGFLAGS_ORDER_MASK) >> PGFLAGS_ORDER_EXP
    );
}

enum block_type buddy_get_type(void *ptr) {
    pmm_page_t *page_meta = pmm_page_struct(((size_t)ptr - vmm_hhdm_offset) / CONFIG_PAGE_SIZE);
    switch ((page_usage_t)((page_meta->flags & PGFLAGS_USAGE_MASK) >> PGFLAGS_USAGE_EXP)) {
        case PAGE_USAGE_FREE: return BLOCK_TYPE_FREE;
        case PAGE_USAGE_PAGE_TABLE:
        case PAGE_USAGE_KERNEL_ANON:
        case PAGE_USAGE_UNUSABLE:
        case PAGE_USAGE_CACHE:
        default: return BLOCK_TYPE_PAGE;
        case PAGE_USAGE_KERNEL_SLAB: return BLOCK_TYPE_SLAB;
        case PAGE_USAGE_MMAP:
        case PAGE_USAGE_USER_ANON: return BLOCK_TYPE_USER;
    }
}

size_t buddy_get_size(void *ptr) {
    pmm_page_t *page_meta = pmm_page_struct(((size_t)ptr - vmm_hhdm_offset) / CONFIG_PAGE_SIZE);
    return 1llu << ((page_meta->flags & PGFLAGS_ORDER_MASK) >> PGFLAGS_ORDER_EXP);
}
