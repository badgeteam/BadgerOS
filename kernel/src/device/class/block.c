
// SPDX-FileCopyrightText: 2025 Julian Scheffers <julian@scheffers.net>
// SPDX-FileType: SOURCE
// SPDX-License-Identifier: MIT

#include "device/class/block.h"

#include "assertions.h"
#include "badge_strings.h"
#include "cpu/interrupt.h"
#include "log.h"
#include "malloc.h"
#include "mutex.h"
#include "radixtree.h"
#include "rcu.h"
#include "scheduler/scheduler.h"
#include "time.h"

#include <stdint.h>

// Untag a block cache pointer.
#define BLK_UNTAG(ptr)      ((__typeof__(ptr))(((size_t)ptr) & ~(size_t)3))
// Tag a block cache pointer as locked.
#define BLK_TAG_LOCKED(ptr) ((__typeof__(ptr))((size_t)(ptr) | 1))
// Tag a block cache pointer as dirty.
#define BLK_TAG_DIRTY(ptr)  ((__typeof__(ptr))((size_t)(ptr) | 2))
// NULL pointer tagged as locked.
#define BLK_LOCKED_NULL     BLK_TAG_LOCKED(NULL)
// Whether a block cache pointer is tagged as locked.
#define BLK_IS_LOCKED(ptr)  (((size_t)(ptr) & 1) != 0)
// Whether a block cache pointer is tagged as dirty.
#define BLK_IS_DIRTY(ptr)   (((size_t)(ptr) & 2) != 0)



// Initialize this block device's cache.
// Must be called by all block devices, during the `driver->add` function, if the device does not have `no_cache` set.
void device_block_init_cache(device_block_t *device) {
    rtree_init(&device->cache);
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
    return device_block_write_bytes(device, start << device->block_size_exp, count << device->block_size_exp, data);
}

// Read device blocks.
// The alignment for DMA is handled by this function.
errno_t device_block_read_blocks(device_block_t *device, uint64_t start, uint64_t count, void *data) {
    return device_block_read_bytes(device, start << device->block_size_exp, count << device->block_size_exp, data);
}

// Erase blocks.
errno_t device_block_erase_blocks(device_block_t *device, uint64_t start, uint64_t count) {
    if (start + count < start || start + count > device->block_count) {
        logkf(
            LOG_WARN,
            "OOB block device access rejected; erasing 0x%{u64;x} @ 0x%{u64;x} on a 0x%{u64;x}x0x%{u64;x} device",
            start << device->block_size_exp,
            count << device->block_size_exp,
            device->block_count,
            1llu << device->block_size_exp
        );
        return -EINVAL;
    }

    logk(LOG_DEBUG, "TODO: device_block_erase_blocks");

    return 0;
}

// Helper function that iterates over a range of cached blocks, reading them from disk if needed.
// Uses the driver functions without checking; that is up to the caller.
static errno_t iterate_block_ranges(
    device_block_t *device,
    uint64_t        start_byte,
    uint64_t        byte_count,
    void (*callback)(uint8_t *subblock_data, size_t subblock_len, size_t byte_offset, void *cookie),
    void *cookie,
    bool  mark_dirty
) {
    driver_block_t const *const driver = (void *)device->base.driver;
    assert_dev_keep(irq_disable());
    rcu_crit_enter();

    // Get the offsets in blocks.
    uint64_t       start_block = start_byte >> device->block_size_exp;
    uint64_t       end_block   = ((start_byte + byte_count - 1) >> device->block_size_exp) + 1;
    uint64_t const block_size  = 1llu << device->block_size_exp;

    for (uint64_t block = start_block; block < end_block; block++) {
        // Get sub-block offsets.
        uint64_t sub_offset;
        uint64_t sub_size;
        if (block == start_block) {
            sub_offset = start_byte & (device->block_size_exp - 1);
            sub_size   = byte_count < block_size - sub_offset ? byte_count : block_size - sub_offset;
        } else {
            sub_offset = 0;
            sub_size   = byte_count - sub_offset < block_size ? byte_count - sub_offset : block_size;
        }

        // Read or allocate the cache entry.
        void *value;
        while (1) {
            // Get the value.
            void *raw_value = rtree_get(&device->cache, block);
            while (BLK_IS_LOCKED(raw_value)) {
                rcu_crit_exit();
                thread_yield();
                irq_disable();
                rcu_crit_enter();
                raw_value = rtree_get(&device->cache, block);
            }
            value = BLK_UNTAG(raw_value);
            if (value) {
                if (mark_dirty) {
                    // Try to mark the page as dirty.
                    if (raw_value != rtree_cmpxchg(&device->cache, block, raw_value, BLK_TAG_DIRTY(raw_value)).ptr) {
                        // Retry if doing so failed.
                        continue;
                    }
                }
                // It's resident in the cache; break out of this loop so it can be accessed.
                break;
            }

            // If NULL, try to lock so we can read the disk.
            errno_ptr_t res = rtree_cmpxchg(&device->cache, block, NULL, BLK_LOCKED_NULL);
            if (res.errno < 0) {
                rcu_crit_exit();
                irq_enable();
                return res.errno;
            }
            if (!BLK_IS_LOCKED(res.ptr)) {
                value = BLK_UNTAG(res.ptr);
                if (value) {
                    // Some other thread put this in the cache in the mean time.
                    break;
                }

                // We've locked an empty cache entry, now read from disk into a new entry.
                rcu_crit_exit();
                irq_enable();
                value = ENOMEM_ON_NULL(malloc(1 << device->block_size_exp), rtree_set(&device->cache, block, NULL));

                // Succeeded to allocate a cache entry, perform actual read.
                RETURN_ON_ERRNO(driver->read_blocks(device, block, 1, value), {
                    free(value);
                    rtree_set(&device->cache, block, NULL);
                });

                // Successfully read everything, now update the entry in the cache to this newly read one.
                irq_disable();
                rcu_crit_enter();
                rtree_set(&device->cache, block, BLK_TAG_DIRTY(value));
                break;
            }
        }

        // Run the callback function with the acquired cache entry.
        callback(value + sub_offset, sub_size, start_byte, cookie);
        start_byte += sub_size;
        byte_count -= sub_size;
    }

    rcu_crit_exit();
    irq_enable();
    return -ENOSYS;
}

// Implementation of `device_block_write_bytes` after caching.
static void write_bytes_cb(uint8_t *subblock_data, size_t subblock_len, size_t byte_offset, void *cookie) {
    uint8_t const *wdata = cookie;
    mem_copy(subblock_data, wdata + byte_offset, subblock_len);
}

// Write block device bytes.
// The alignment for DMA is handled by this function.
errno_t device_block_write_bytes(device_block_t *device, uint64_t offset, uint64_t size, void const *data) {
    if (offset + size < offset || offset + size > device->block_count << device->block_size_exp) {
        logkf(
            LOG_WARN,
            "OOB block device access rejected; writing 0x%{u64;x} @ 0x%{u64;x} on a 0x%{u64;x}x0x%{u64;x} device",
            offset,
            size,
            device->block_count,
            1llu << device->block_size_exp
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

    iterate_block_ranges(device, offset, size, write_bytes_cb, (void *)data, true);

    mutex_release_shared(&device->base.driver_mtx);
    return 0;
}

// Implementation of `device_block_read_bytes` after caching.
static void read_bytes_cb(uint8_t *subblock_data, size_t subblock_len, size_t byte_offset, void *cookie) {
    uint8_t *rdata = cookie;
    mem_copy(rdata + byte_offset, subblock_data, subblock_len);
}

// Read block device bytes.
// The alignment for DMA is handled by this function.
errno_t device_block_read_bytes(device_block_t *device, uint64_t offset, uint64_t size, void *data0) {
    uint8_t *data = data0;
    if (offset + size < offset || offset + size > device->block_count << device->block_size_exp) {
        logkf(
            LOG_WARN,
            "OOB block device access rejected; writing 0x%{u64;x} @ 0x%{u64;x} on a 0x%{u64;x}x0x%{u64;x} device",
            offset,
            size,
            device->block_count,
            1llu << device->block_size_exp
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

    iterate_block_ranges(device, offset, size, read_bytes_cb, (void *)data, false);

    mutex_release_shared(&device->base.driver_mtx);
    return 0;
}

// Erase block device bytes.
errno_t device_block_erase_bytes(device_block_t *device, uint64_t offset, uint64_t size) {
    uint64_t const block_size  = 1llu << device->block_size_exp;
    uint64_t       end_offset  = offset + size;
    uint64_t       start_block = (offset + block_size - 1) >> device->block_size_exp;
    uint64_t       end_block   = end_offset >> device->block_size_exp;
    return device_block_erase_blocks(device, start_block, end_block - start_block);
}



// Apply all pending changes.
errno_t device_block_sync_all(device_block_t *device, bool flush) {
    return device_block_sync_blocks(device, 0, device->block_count, flush);
}

// Apply pending changes in a range of blocks.
errno_t device_block_sync_blocks(device_block_t *device, uint64_t start, uint64_t count, bool flush) {
    mutex_acquire_shared(&device->base.driver_mtx, TIMESTAMP_US_MAX);
    if (!device->base.driver) {
        mutex_release_shared(&device->base.driver_mtx);
        return -ENOENT;
    }
    mutex_release_shared(&device->base.driver_mtx);
    return 0;
}

// Apply pending changes in a range of bytes.
errno_t device_block_sync_bytes(device_block_t *device, uint64_t offset, uint64_t size, bool flush) {
    uint64_t const block_size  = 1llu << device->block_size_exp;
    size                      += offset & (block_size - 1);
    offset                    -= offset & (block_size - 1);
    if (size & (block_size - 1)) {
        size += block_size - (size & (block_size - 1));
    }
    return device_block_sync_blocks(device, offset >> device->block_size_exp, size >> device->block_size_exp, flush);
}
