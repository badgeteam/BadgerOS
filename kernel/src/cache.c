
// SPDX-FileCopyrightText: 2025 Julian Scheffers <julian@scheffers.net>
// SPDX-FileType: SOURCE
// SPDX-License-Identifier: MIT

#include "cache.h"

#include "assertions.h"
#include "malloc.h"
#include "mutex.h"
#include "refcount.h"
#include "time.h"
#include "todo.h"



// Remove all data from the cache.
void cache_clear(cache_t *cache) {
    TODO();
}



// Remove an entry.
// Will wait for the entry to be unlocked before removal.
cache_data_t cache_remove(cache_t *cache, uint64_t index) {
    TODO();
}

// Read an entry.
// If there is a data block, it will clone the refcount pointer to it.
cache_data_t cache_get(cache_t *cache, uint64_t index) {
    TODO();
}

// Mark an entry as dirty.
bool cache_mark_dirty(cache_t *cache, uint64_t index) {
    TODO();
}



// Lock an entry, creating it if it doesn't exist.
bool cache_lock(cache_t *cache, uint64_t index, timestamp_us_t timeout) {
    TODO();
}

// Unlock an entry.
void cache_unlock(cache_t *cache, uint64_t index) {
    TODO();
}

// Unlock then immediately remove it.
void cache_unlock_remove(cache_t *cache, uint64_t index) {
    TODO();
}

// Read an entry without locking. The entry must already be locked.
// If there is a data block, it will clone the refcount pointer to it.
cache_data_t cache_get_unsafe(cache_t *cache, uint64_t index) {
    TODO();
}

// Update an entry. The entry must be locked.
bool cache_set_unsafe(cache_t *cache, uint64_t index, cache_data_t new_data) {
    TODO();
}

// Mark an entry as clear. The entry must be locked.
bool cache_mark_clean_unsafe(cache_t *cache, uint64_t index) {
    TODO();
}
