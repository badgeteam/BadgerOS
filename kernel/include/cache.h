
// SPDX-FileCopyrightText: 2025 Julian Scheffers <julian@scheffers.net>
// SPDX-FileType: SOURCE
// SPDX-License-Identifier: MIT

#pragma once

#include "mutex.h"
#include "radixtree.h"
#include "refcount.h"
#include "time.h"



// A generic thread-safe cache for blocks/pages of data.
typedef struct cache       cache_t;
// A page/block's worth of cached data.
typedef struct cache_data  cache_data_t;
// An entry of the cache.
typedef struct cache_entry cache_entry_t;

// A generic thread-safe cache for blocks/pages of data.
struct cache {
    // Mutex protecting the radix tree.
    mutex_t mtx;
    // Radix tree of cache entries.
    rtree_t entries;
    // Size of the data blocks.
    size_t  block_size;
};

// A page/block's worth of cached data.
struct cache_data {
    // Entry is valid (contains data).
    bool valid;
    // Entry is dirty (differs from disk).
    bool is_dirty;
    // Refcount ptr to the current data block.
    rc_t buffer;
};

// An entry in the cache, protected by a mutex.
struct cache_entry {
    // Mutex protecting this entry for the purpose of disk access.
    // Taken shared for in-memory operations, exclusive for disk access.
    mutex_t      mtx;
    // Actual data stored here.
    cache_data_t data;
};

#define CACHE_T_INIT(_addr_width, _block_size) {MUTEX_T_INIT_SHARED, RTREE_T_INIT, _block_size}



// Remove all data from the cache.
void cache_clear(cache_t *cache);

// Remove an entry.
// Will wait for the entry to be unlocked before removal.
cache_data_t cache_remove(cache_t *cache, uint64_t index);
// Read an entry.
// If there is a data block, it will clone the refcount pointer to it.
cache_data_t cache_get(cache_t *cache, uint64_t index);
// Mark an entry as dirty.
bool         cache_mark_dirty(cache_t *cache, uint64_t index);

// Lock an entry, creating it if it doesn't exist.
bool         cache_lock(cache_t *cache, uint64_t index, timestamp_us_t timeout);
// Unlock an entry.
void         cache_unlock(cache_t *cache, uint64_t index);
// Unlock then immediately remove it.
void         cache_unlock_remove(cache_t *cache, uint64_t index);
// Read an entry without locking. The entry must already be locked.
// If there is a data block, it will clone the refcount pointer to it.
cache_data_t cache_get_unsafe(cache_t *cache, uint64_t index);
// Update an entry. The entry must be locked.
bool         cache_set_unsafe(cache_t *cache, uint64_t index, cache_data_t new_data);
// Mark an entry as cleam. The entry must be locked.
bool         cache_mark_clean_unsafe(cache_t *cache, uint64_t index);
