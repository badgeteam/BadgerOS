
// SPDX-FileCopyrightText: 2025 Julian Scheffers <julian@scheffers.net>
// SPDX-FileType: SOURCE
// SPDX-License-Identifier: MIT

#include "device/class/block.h"

#include "malloc.h"
#include "mutex.h"



// Block device cache entry.
typedef struct {
    // Actual device block index.
    uint64_t block;
    // Entry is present.
    bool     is_present;
    // Entry is dirty (cache newer than device).
    bool     is_dirty;
    // Entry is erase, not data.
    bool     is_erase;
    union {
        // Data in this block.
        uint8_t       *data;
        // Block erase mode.
        blkdev_erase_t erase_mode;
    };
} blk_cache_line_t;

// Block device cache.
struct blk_cache {
    // Cache mutex; taken exclusive for syncing, shared for read/write ops.
    mutex_t           mtx;
    // Number of entries; must be a power of 2.
    size_t            lines_len;
    // Cache entries.
    blk_cache_line_t *lines;
};



// Initialize a block device's cache.
// Number of lines must be a power of 2 at least 4.
// Optional for block devices with byte access methods.
bool device_block_init_cache(device_block_t *device, size_t num_lines) {
    // Allocate memory for the cache.
    blk_cache_t *cache = malloc(sizeof(blk_cache_t));
    if (!cache) {
        return false;
    }

    cache->lines_len = num_lines;
    cache->lines     = calloc(num_lines, sizeof(blk_cache_line_t));
    if (!cache->lines) {
        free(cache);
        return false;
    }

    mutex_init(&cache->mtx, true);
    return true;
}

// Clean up a block device's cache.
// Implicitly called by the device subsystem if a `device_block_t` is freed.
void device_block_free_cache(device_block_t *device) {
    blk_cache_t *cache = device->cache;
    if (!cache) {
        return;
    }
    device->cache = NULL;

    for (size_t i = 0; i < cache->lines_len; i++) {
        if (cache->lines[i].is_present && !cache->lines[i].is_erase) {
            free(cache->lines[i].data);
        }
    }
    free(cache->lines);
    free(cache);
}



// Write device blocks.
// The caller must ensure that `data` is aligned at least as much as needed for DMA.
bool device_block_write_blocks(device_block_t *device, uint64_t start, uint64_t count, void const *data) {
    __builtin_trap();
}

// Read device blocks.
// The caller must ensure that `data` is aligned at least as much as needed for DMA.
bool device_block_read_blocks(device_block_t *device, uint64_t start, uint64_t count, void *data) {
    __builtin_trap();
}

// Erase blocks.
bool device_block_erase_blocks(device_block_t *device, uint64_t start, uint64_t count, blkdev_erase_t mode) {
    __builtin_trap();
}

// Write block device bytes.
// The alignment for DMA is handled by this function.
bool device_block_write_bytes(device_block_t *device, uint64_t offset, uint64_t size, void const *data) {
    __builtin_trap();
}

// Read block device bytes.
// The alignment for DMA is handled by this function.
bool device_block_read_bytes(device_block_t *device, uint64_t offset, uint64_t size, void *data) {
    __builtin_trap();
}

// Erase block device bytes.
bool device_block_erase_bytes(device_block_t *device, uint64_t offset, uint64_t size, blkdev_erase_t mode) {
    __builtin_trap();
}



// Apply all pending changes.
bool device_block_sync_all(device_block_t *device) {
    __builtin_trap();
}

// Apply pending changes in a range of blocks.
bool device_block_sync_blocks(device_block_t *device, uint64_t start, uint64_t count) {
    __builtin_trap();
}

// Apply pending changes in a range of bytes.
bool device_block_sync_bytes(device_block_t *device, uint64_t offset, uint64_t size) {
    __builtin_trap();
}
