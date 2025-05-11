
// SPDX-FileCopyrightText: 2025 Julian Scheffers <julian@scheffers.net>
// SPDX-FileType: SOURCE
// SPDX-License-Identifier: MIT

#include "cache.h"

#include "assertions.h"
#include "malloc.h"
#include "mutex.h"
#include "radixtree.h"
#include "refcount.h"
#include "time.h"



// Remove all data from the cache.
void cache_clear(cache_t *cache) {
    mutex_acquire(&cache->mtx, TIMESTAMP_US_MAX);

    rtree_foreach(iter, &cache->entries) {
        cache_entry_t *ent = iter.value;
        mutex_destroy(&ent->mtx);
        if (ent->data.buffer) {
            rc_delete(ent->data.buffer);
        }
    }
    rtree_clear(&cache->entries);

    mutex_release(&cache->mtx);
}



// Remove an entry.
// Will wait for the entry to be unlocked before removal.
cache_data_t cache_remove(cache_t *cache, uint64_t index) {
    mutex_acquire(&cache->mtx, TIMESTAMP_US_MAX);

    cache_entry_t *ent = rtree_get(&cache->entries, index);
    if (!ent) {
        mutex_release(&cache->mtx);
        return (cache_data_t){0};
    }

    // Dummy acquire of the mutex; because the cache mutex is also held exclusively, it cannot be reacquired.
    // Most likely, this is effectively no-op as it is exceedingly unlikely that an entry is also being synced.
    mutex_acquire(&ent->mtx, TIMESTAMP_US_MAX);
    mutex_release(&ent->mtx);

    // It's now safe to remove.
    cache_data_t data = ent->data;
    mutex_destroy(&ent->mtx);
    rtree_remove(&cache->entries, index);

    mutex_release(&cache->mtx);

    return data;
}

// Read an entry.
// If there is a data block, it will clone the refcount pointer to it.
cache_data_t cache_get(cache_t *cache, uint64_t index) {
    mutex_acquire_shared(&cache->mtx, TIMESTAMP_US_MAX);

    // Look up the entry.
    cache_entry_t *ent = rtree_get(&cache->entries, index);
    if (!ent) {
        mutex_release_shared(&cache->mtx);
        return (cache_data_t){0};
    }

    // Copy the data out.
    mutex_acquire_shared(&ent->mtx, TIMESTAMP_US_MAX);
    cache_data_t data = ent->data;
    rc_share(data.buffer);
    mutex_release_shared(&ent->mtx);

    mutex_release_shared(&cache->mtx);

    return data;
}

// Mark an entry as dirty.
bool cache_mark_dirty(cache_t *cache, uint64_t index) {
    mutex_acquire_shared(&cache->mtx, TIMESTAMP_US_MAX);

    // Look up the entry.
    cache_entry_t *ent = rtree_get(&cache->entries, index);
    if (!ent) {
        mutex_release_shared(&cache->mtx);
        return false;
    }

    // Update the entry.
    mutex_acquire_shared(&ent->mtx, TIMESTAMP_US_MAX);
    ent->data.is_dirty = true;
    mutex_release_shared(&ent->mtx);

    mutex_release_shared(&cache->mtx);

    return true;
}



// Lock an entry, creating it if it doesn't exist.
bool cache_lock(cache_t *cache, uint64_t index, timestamp_us_t timeout) {
    mutex_acquire_shared(&cache->mtx, TIMESTAMP_US_MAX);

    // Look up the entry.
    cache_entry_t *ent = rtree_get(&cache->entries, index);
    if (!ent) {
        // Entry doesn't exist; try to make a new one.
        void *raw_buf = malloc(cache->block_size);
        if (!raw_buf) {
            mutex_release_shared(&cache->mtx);
            return false;
        }
        cache_entry_t new_ent = {
            .data.valid  = false,
            .data.buffer = rc_new(raw_buf, free),
        };
        if (!new_ent.data.buffer) {
            free(raw_buf);
            mutex_release_shared(&cache->mtx);
            return false;
        }
        if (!rtree_set(&cache->entries, index, &new_ent)) {
            rc_delete(new_ent.data.buffer);
            mutex_release_shared(&cache->mtx);
            return false;
        }
    }

    mutex_acquire(&ent->mtx, TIMESTAMP_US_MAX);

    mutex_release_shared(&cache->mtx);

    return true;
}

// Unlock an entry.
void cache_unlock(cache_t *cache, uint64_t index) {
    mutex_acquire_shared(&cache->mtx, TIMESTAMP_US_MAX);

    // Look up the entry.
    cache_entry_t *ent = rtree_get(&cache->entries, index);
    assert_always(ent);

    mutex_release(&ent->mtx);

    mutex_release_shared(&cache->mtx);
}

// Unlock then immediately remove it.
void cache_unlock_remove(cache_t *cache, uint64_t index) {
    mutex_acquire(&cache->mtx, TIMESTAMP_US_MAX);

    // Look up the entry.
    cache_entry_t *ent = rtree_get(&cache->entries, index);
    assert_always(ent);

    mutex_release(&ent->mtx);
    mutex_destroy(&ent->mtx);
    if (ent->data.buffer) {
        rc_delete(ent->data.buffer);
    }

    mutex_release(&cache->mtx);
}

// Read an entry without locking. The entry must already be locked.
// If there is a data block, it will clone the refcount pointer to it.
cache_data_t cache_get_unsafe(cache_t *cache, uint64_t index) {
    mutex_acquire_shared(&cache->mtx, TIMESTAMP_US_MAX);

    // Look up the entry.
    cache_entry_t *ent = rtree_get(&cache->entries, index);
    if (!ent) {
        mutex_release_shared(&cache->mtx);
        return (cache_data_t){0};
    }

    // Copy the data out.
    cache_data_t data = ent->data;
    rc_share(data.buffer);

    mutex_release_shared(&cache->mtx);

    return data;
}

// Update an entry. The entry must be locked.
bool cache_set_unsafe(cache_t *cache, uint64_t index, cache_data_t new_data) {
    mutex_acquire_shared(&cache->mtx, TIMESTAMP_US_MAX);

    // Look up the entry.
    cache_entry_t *ent = rtree_get(&cache->entries, index);
    if (!ent) {
        mutex_release_shared(&cache->mtx);
        return false;
    }

    // Replace the entry.
    mutex_acquire(&ent->mtx, TIMESTAMP_US_MAX);
    if (ent->data.buffer) {
        rc_delete(ent->data.buffer);
    }
    ent->data = new_data;
    mutex_release(&ent->mtx);

    mutex_release_shared(&cache->mtx);

    return true;
}

// Mark an entry as clear. The entry must be locked.
bool cache_mark_clean_unsafe(cache_t *cache, uint64_t index) {
    mutex_acquire_shared(&cache->mtx, TIMESTAMP_US_MAX);

    // Look up the entry.
    cache_entry_t *ent = rtree_get(&cache->entries, index);
    if (!ent) {
        mutex_release_shared(&cache->mtx);
        return false;
    }

    ent->data.is_dirty = false;

    mutex_release_shared(&cache->mtx);

    return true;
}
