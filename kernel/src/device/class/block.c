
// SPDX-FileCopyrightText: 2025 Julian Scheffers <julian@scheffers.net>
// SPDX-FileType: SOURCE
// SPDX-License-Identifier: MIT

#include "device/class/block.h"

#include "mutex.h"



// Block device cache entry.
typedef struct {
    // Cache line mutex.
    // Taken exclusive for syncing, shared for read/write ops.
    mutex_t  mtx;
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
typedef struct {
    // Number of entries; must be a power of 2.
    size_t            lines_len;
    // Cache entries.
    blk_cache_line_t *lines;
} blk_cache_t;



// Get device block size.
uint64_t device_block_block_size(device_t *device);
// Get number of blocks.
uint64_t device_block_block_count(device_t *device);
// Get device DMA alignment.
size_t   device_block_dma_align(device_t *device);
// Write device blocks.
// The caller must ensure that `data` is aligned at least as much as needed for DMA.
void     device_block_write_blocks(device_t *device, uint64_t start, uint64_t count, void const *data);
// Read device blocks.
// The caller must ensure that `data` is aligned at least as much as needed for DMA.
void     device_block_read_blocks(device_t *device, uint64_t start, uint64_t count, void *data);
// Erase blocks.
void     device_block_erase_blocks(device_t *device, uint64_t start, uint64_t count, blkdev_erase_t mode);

// Apply all pending changes.
void device_block_sync_all(device_t *device);
// Apply pending changes in a range of blocks.
void device_block_sync_blocks(device_t *device, uint64_t start, uint64_t count);
// Apply pending changes in a range of bytes.
void device_block_sync_bytes(device_t *device, uint64_t offset, uint64_t size);

// Write block device bytes.
// The alignment for DMA is handled by this function.
void device_block_write_bytes(device_t *device, uint64_t offset, uint64_t size, void const *data);
// Read block device bytes.
// The alignment for DMA is handled by this function.
void device_block_read_bytes(device_t *device, uint64_t offset, uint64_t size, void *data);
// Erase block device bytes.
void device_block_erase_bytes(device_t *device, uint64_t offset, uint64_t size, blkdev_erase_t mode);
