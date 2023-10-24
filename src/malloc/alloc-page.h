#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdatomic.h>

#include "skiplist.h"
#include "debug.h"
#include "compiler.h"

#define PAGE_SIZE 4096

typedef struct {
    void* pages_start;
    atomic_uintptr_t pages_end;
    atomic_size_t pages;

    skiplist_t pages_list;

    bool grows_up;
} page_pool_t;

typedef struct {
    void* mem_start;
    void* mem_end;

    void* pages_start;
    void* pages_end;

    size_t total_mem_size;
    size_t pages;

    void* kernel_user_split;
} page_alloc_state_t;

FORCEINLINE void* get_page_address(page_pool_t* pool, size_t index) {
    BADGEROS_MALLOC_ASSERT_ERROR(index < pool->pages, "Trying to get index of page past end: index: %zi, pool size: %zi", index, pool->pages);

    if (pool->grows_up) {
        return pool->pages_start + (index * PAGE_SIZE);
    } else {
        return pool->pages_start - (index * PAGE_SIZE);
    }
}

