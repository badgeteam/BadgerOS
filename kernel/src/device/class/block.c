
// SPDX-FileCopyrightText: 2025 Julian Scheffers <julian@scheffers.net>
// SPDX-FileType: SOURCE
// SPDX-License-Identifier: MIT

#include "device/class/block.h"

#include "assertions.h"
#include "badge_strings.h"
#include "cache.h"
#include "log.h"
#include "malloc.h"
#include "mutex.h"
#include "radixtree.h"
#include "refcount.h"
#include "time.h"

#include <stdint.h>



// Initialize this block device's cache.
// Must be called by all block devices, during the `driver->add` function, if the device does not have `no_cache` set.
void device_block_init_cache(device_block_t *device) {
    device->cache = (cache_t)CACHE_T_INIT(64 - __builtin_clzll(device->block_count), device->block_size);
}



// Create a block device file with a certain prefix.
// The prefix must not end in a digit because disks are disambiguated by appending a number.
// Similarly, partitions are disambiguated by appending 'p' and then a number.
errno_t device_block_create_blkfile(device_block_t *device) {
    return -ENOSYS;
}



// Write device blocks.
// The alignment for DMA is handled by this function.
errno_t device_block_write_blocks(device_block_t *device, uint64_t start, uint64_t count, void const *data) {
    return device_block_write_bytes(device, start * device->block_size, count * device->block_size, data);
}

// Read device blocks.
// The alignment for DMA is handled by this function.
errno_t device_block_read_blocks(device_block_t *device, uint64_t start, uint64_t count, void *data) {
    return device_block_read_bytes(device, start * device->block_size, count * device->block_size, data);
}

// Erase blocks.
errno_t device_block_erase_blocks(device_block_t *device, uint64_t start, uint64_t count, blkdev_erase_t mode) {
    return device_block_erase_bytes(device, start * device->block_size, count * device->block_size, mode);
}

// Write block device bytes.
// The alignment for DMA is handled by this function.
errno_t device_block_write_bytes(device_block_t *device, uint64_t offset, uint64_t size, void const *data0) {
    uint8_t const *data = data0;
    if (offset + size < offset || offset + size > device->block_size * device->block_count) {
        logkf(
            LOG_WARN,
            "OOB block device access rejected; writing 0x%{u64;x} @ 0x%{u64;x} on a 0x%{u64;x}x0x%{u64;x} device",
            offset,
            size,
            device->block_count,
            device->block_size
        );
        return -EINVAL;
    }

    mutex_acquire_shared(&device->base.driver_mtx, TIMESTAMP_US_MAX);
    driver_block_t const *driver = (void *)device->base.driver;
    if (device->no_cache) {
        errno_t res = driver->write_bytes(device, offset, size, data);
        mutex_release_shared(&device->base.driver_mtx);
        return res;
    }

    // Get the offsets in blocks.
    uint64_t start_block = offset / device->block_size;
    uint64_t end_block   = (offset + size - 1) / device->block_size + 1;

    uint64_t init_offset = offset;
    for (uint64_t i = start_block; i < end_block; i++) {
        // Get sub-block offsets.
        uint64_t sub_offset;
        uint64_t sub_size;
        if (i == start_block) {
            sub_offset = offset % device->block_size;
            sub_size   = size < device->block_size - sub_offset ? size : device->block_size - sub_offset;
        } else {
            sub_offset = 0;
            sub_size   = size - sub_offset < device->block_size ? size - sub_offset : device->block_size;
        }

        // Try to read the data from the cache.
        cache_data_t ent = cache_get(&device->cache, i);
        if (ent.valid) {
            // Data present in cache; copy into it.
            mem_copy(ent.buffer->data + sub_offset, data + offset - init_offset, sub_size);
            rc_delete(ent.buffer);

        } else {
            // Try to cache a read from the device, then write.
            if (!cache_lock(&device->cache, i, TIMESTAMP_US_MAX)) {
                mutex_release_shared(&device->base.driver_mtx);
                return -EINVAL;
            }

            cache_data_t existing = cache_get_unsafe(&device->cache, i);
            if (existing.valid) {
                // Some other thread read it first; copy into it as normal.
                mem_copy(existing.buffer->data + sub_offset, data + offset - init_offset, sub_size);
                rc_delete(existing.buffer);

            } else {
                // Cache is indeed still empty; make new entry.
                void *raw_buf = malloc(device->block_size);
                if (!raw_buf) {
                    cache_unlock(&device->cache, i);
                    mutex_release_shared(&device->base.driver_mtx);
                    return -ENOMEM;
                }
                cache_data_t new_ent = {
                    .valid    = true,
                    .is_dirty = false,
                    .buffer   = rc_new(raw_buf, free),
                };
                if (!new_ent.buffer) {
                    free(raw_buf);
                    cache_unlock(&device->cache, i);
                    mutex_release_shared(&device->base.driver_mtx);
                    return -ENOMEM;
                }

                // Read the data and copy into it.
                errno_t res = driver->read_blocks(device, i, 1, new_ent.buffer->data);
                if (res < 0) {
                    rc_delete(new_ent.buffer);
                    cache_unlock(&device->cache, i);
                    mutex_release_shared(&device->base.driver_mtx);
                    return res;
                }
                mem_copy(new_ent.buffer->data + sub_offset, data + offset - init_offset, sub_size);

                // Try to insert the data into the cache.
                if (!cache_set_unsafe(&device->cache, i, new_ent)) {
                    rc_delete(new_ent.buffer);
                    cache_unlock(&device->cache, i);
                    mutex_release_shared(&device->base.driver_mtx);
                    return -ENOMEM;
                }
            }

            cache_unlock(&device->cache, i);
        }

        // Increment offsets.
        offset += sub_size;
        size   -= sub_size;
    }

    mutex_release_shared(&device->base.driver_mtx);
    return 0;
}

// Read block device bytes.
// The alignment for DMA is handled by this function.
errno_t device_block_read_bytes(device_block_t *device, uint64_t offset, uint64_t size, void *data0) {
    uint8_t *data = data0;
    if (offset + size < offset || offset + size > device->block_size * device->block_count) {
        logkf(
            LOG_WARN,
            "OOB block device access rejected; reading 0x%{u64;x} @ 0x%{u64;x} on a 0x%{u64;x}x0x%{u64;x} device",
            offset,
            size,
            device->block_count,
            device->block_size
        );
        return -EINVAL;
    }

    mutex_acquire_shared(&device->base.driver_mtx, TIMESTAMP_US_MAX);
    driver_block_t const *driver = (void *)device->base.driver;
    if (device->no_cache) {
        errno_t res = driver->read_bytes(device, offset, size, data);
        mutex_release_shared(&device->base.driver_mtx);
        return res;
    }

    // Get the offsets in blocks.
    uint64_t start_block = offset / device->block_size;
    uint64_t end_block   = (offset + size - 1) / device->block_size + 1;

    uint64_t init_offset = offset;
    for (uint64_t i = start_block; i < end_block; i++) {
        // Get sub-block offsets.
        uint64_t sub_offset;
        uint64_t sub_size;
        if (i == start_block) {
            sub_offset = offset % device->block_size;
            sub_size   = size < device->block_size - sub_offset ? size : device->block_size - sub_offset;
        } else {
            sub_offset = 0;
            sub_size   = size - sub_offset < device->block_size ? size - sub_offset : device->block_size;
        }

        // Try to read the data from the cache.
        cache_data_t ent = cache_get(&device->cache, i);
        if (ent.valid) {
            // Data present in cache; copy from it.
            mem_copy(data + offset - init_offset, ent.buffer->data + sub_offset, sub_size);
            rc_delete(ent.buffer);

        } else if (device->no_read_cache) {
            // Device wants no read caching; ask it to read directly.
            driver->read_bytes(device, offset, sub_size, data + offset - init_offset);

        } else {
            // Try to cache a read from the device.
            if (!cache_lock(&device->cache, i, TIMESTAMP_US_MAX)) {
                mutex_release_shared(&device->base.driver_mtx);
                return -EINVAL;
            }

            cache_data_t existing = cache_get_unsafe(&device->cache, i);
            if (existing.valid) {
                // Some other thread read it first; copy out as normal.
                mem_copy(data + offset - init_offset, existing.buffer->data + sub_offset, sub_size);
                rc_delete(existing.buffer);

            } else {
                // Cache is indeed still empty; make new entry.
                void *raw_buf = malloc(device->block_size);
                if (!raw_buf) {
                    mutex_release_shared(&device->base.driver_mtx);
                    cache_unlock(&device->cache, i);
                    mutex_release_shared(&device->base.driver_mtx);
                    return -ENOMEM;
                }
                cache_data_t new_ent = {
                    .valid    = true,
                    .is_dirty = false,
                    .buffer   = rc_new(raw_buf, free),
                };
                if (!new_ent.buffer) {
                    free(raw_buf);
                    mutex_release_shared(&device->base.driver_mtx);
                    cache_unlock(&device->cache, i);
                    mutex_release_shared(&device->base.driver_mtx);
                    return -ENOMEM;
                }

                // Read the data and copy it out.
                errno_t res = driver->read_blocks(device, i, 1, new_ent.buffer->data);
                if (res < 0) {
                    rc_delete(new_ent.buffer);
                    cache_unlock(&device->cache, i);
                    mutex_release_shared(&device->base.driver_mtx);
                    return res;
                }
                mem_copy(data + offset - init_offset, new_ent.buffer->data + sub_offset, sub_size);

                // Try to insert the data into the cache.
                if (!cache_set_unsafe(&device->cache, i, new_ent)) {
                    rc_delete(new_ent.buffer);
                    cache_unlock(&device->cache, i);
                    mutex_release_shared(&device->base.driver_mtx);
                    return -ENOMEM;
                }
            }

            cache_unlock(&device->cache, i);
        }

        // Increment offsets.
        offset += sub_size;
        size   -= sub_size;
    }

    mutex_release_shared(&device->base.driver_mtx);
    return 0;
}

// Erase block device bytes.
errno_t device_block_erase_bytes(device_block_t *device, uint64_t offset, uint64_t size, blkdev_erase_t mode) {
    if (offset + size < offset || offset + size > device->block_size * device->block_count) {
        logkf(
            LOG_WARN,
            "OOB block device access rejected; erasing 0x%{u64;x} @ 0x%{u64;x} on a 0x%{u64;x}x0x%{u64;x} device",
            offset,
            size,
            device->block_count,
            device->block_size
        );
        return -EINVAL;
    }

    mutex_acquire_shared(&device->base.driver_mtx, TIMESTAMP_US_MAX);
    driver_block_t const *driver = (void *)device->base.driver;
    if (device->no_cache) {
        errno_t res = driver->erase_bytes(device, offset, size, mode);
        mutex_release_shared(&device->base.driver_mtx);
        return res;
    }

    // Get the offsets in blocks.
    uint64_t start_block = offset / device->block_size;
    uint64_t end_block   = (offset + size - 1) / device->block_size + 1;

    uint8_t erase_value = mode == BLKDEV_ERASE_NATIVE ? device->erase_value : 0;
    for (uint64_t i = start_block; i < end_block; i++) {
        // Get sub-block offsets.
        uint64_t sub_offset;
        uint64_t sub_size;
        if (i == start_block) {
            sub_offset = offset % device->block_size;
            sub_size   = size < device->block_size - sub_offset ? size : device->block_size - sub_offset;
        } else {
            sub_offset = 0;
            sub_size   = size - sub_offset < device->block_size ? size - sub_offset : device->block_size;
        }

        // Try to read the data from the cache.
        cache_data_t ent = cache_get(&device->cache, i);
        if (ent.valid) {
            // Data present in cache; copy into it.
            mem_set(ent.buffer->data + sub_offset, erase_value, sub_size);
            rc_delete(ent.buffer);

        } else {
            // Try to cache a read from the device, then write.
            if (!cache_lock(&device->cache, i, TIMESTAMP_US_MAX)) {
                mutex_release_shared(&device->base.driver_mtx);
                return -EINVAL;
            }

            // Make new entry.
            void *raw_buf = malloc(device->block_size);
            if (!raw_buf) {
                mutex_release_shared(&device->base.driver_mtx);
                return -ENOMEM;
            }
            cache_data_t new_ent = {
                .valid    = true,
                .is_dirty = false,
                .buffer   = rc_new(raw_buf, free),
            };
            if (!new_ent.buffer) {
                free(raw_buf);
                mutex_release_shared(&device->base.driver_mtx);
                return -ENOMEM;
            }

            // Fill the new buffer with the erase value.
            mem_set(new_ent.buffer->data + sub_offset, erase_value, sub_size);

            // Try to insert the data into the cache.
            if (!cache_set_unsafe(&device->cache, i, new_ent)) {
                rc_delete(new_ent.buffer);
                mutex_release_shared(&device->base.driver_mtx);
                return -ENOMEM;
            }

            cache_unlock(&device->cache, i);
        }

        // Increment offsets.
        offset += sub_size;
        size   -= sub_size;
    }

    mutex_release_shared(&device->base.driver_mtx);
    return 0;
}



// Apply all pending changes.
errno_t device_block_sync_all(device_block_t *device, bool flush) {
    return device_block_sync_blocks(device, 0, device->block_count, flush);
}

static errno_t device_block_sync_block(device_block_t *device, uint64_t block, bool flush) {
    if (block > device->block_count) {
        logkf(LOG_WARN, "OOB block device sync ignored; syncing at 0x%{u64;x}", block * device->block_size);
        return 0;
    }

    if (!cache_lock(&device->cache, block, TIMESTAMP_US_MAX)) {
        return -EINVAL;
    }

    driver_block_t const *driver = (void *)device->base.driver;
    cache_data_t          ent    = cache_get_unsafe(&device->cache, block);
    if (!ent.valid) {
        // No data; no need to sync.
        cache_unlock_remove(&device->cache, block);
        return 0;
    }

    errno_t res = 0;
    if (ent.is_dirty) {
        res = driver->write_blocks(device, block, 1, ent.buffer->data);
        if (res < 0) {
            logkf(
                LOG_WARN,
                "Block device sync failed at 0x%{u64;x}; write data remains cached",
                block * device->block_size
            );
        }
    }
    rc_delete(ent.buffer);

    if (flush && res >= 0) {
        cache_unlock_remove(&device->cache, block);
    } else {
        if (res >= 0) {
            cache_mark_clean_unsafe(&device->cache, block);
        }
        cache_unlock(&device->cache, block);
    }

    return res;
}

// Apply pending changes in a range of blocks.
errno_t device_block_sync_blocks(device_block_t *device, uint64_t start, uint64_t count, bool flush) {
    mutex_acquire_shared(&device->base.driver_mtx, TIMESTAMP_US_MAX);
    if (!device->base.driver) {
        mutex_release_shared(&device->base.driver_mtx);
        return -ENOENT;
    }
    for (; count; start++, count--) {
        errno_t res = device_block_sync_block(device, start, flush);
        if (res < 0) {
            mutex_release_shared(&device->base.driver_mtx);
            return res;
        }
    }
    mutex_release_shared(&device->base.driver_mtx);
    return 0;
}

// Apply pending changes in a range of bytes.
errno_t device_block_sync_bytes(device_block_t *device, uint64_t offset, uint64_t size, bool flush) {
    size   += offset & (device->block_size - 1);
    offset -= offset & (device->block_size - 1);
    if (size & (device->block_size - 1)) {
        size += device->block_size - (size & (device->block_size - 1));
    }
    return device_block_sync_blocks(device, offset / device->block_size, size / device->block_size, flush);
}
