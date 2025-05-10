
// SPDX-FileCopyrightText: 2025 Julian Scheffers <julian@scheffers.net>
// SPDX-FileType: SOURCE
// SPDX-License-Identifier: MIT

#pragma once

#include "mutex.h"
#include "radixtree.h"
#include "refcount.h"



// A generic thread-safe cache for blocks/pages of data.
typedef struct cache       cache_t;
// A page/block's worth of cached data.
typedef struct cache_entry cache_entry_t;

// A generic thread-safe cache for blocks/pages of data.
struct cache {
    // Mutex protecting the radix tree.
    mutex_t entries_mtx;
    // Radix tree of cache entries.
    rtree_t entries;
};

// A page/block's worth of cached data.
struct cache_entry {
    // Mutex protecting this entry for the purpose of disk access.
    // Taken shared for in-memory operations, exclusive for disk access.
    mutex_t disk_mtx;
    // Entry is dirty (differs from disk).
    bool    is_dirty;
    // Entry is erased.
    bool    is_erased;
    // Erase value.
    uint8_t erase_value;
    // Refcount ptr to the current data block.
    rc_t    data;
};
