
// SPDX-FileCopyrightText: 2025 Julian Scheffers <julian@scheffers.net>
// SPDX-FileType: SOURCE
// SPDX-License-Identifier: MIT

#pragma once

#include "device/device.h"



// Block device erase mode.
typedef enum {
    // Erase with device's native erase value.
    BLKDEV_ERASE_NATIVE,
    // Erase and fill with zeroes.
    BLKDEV_ERASE_ZERO,
} blkdev_erase_t;

// Block device cache.
typedef struct blk_cache blk_cache_t;

// Block device.
typedef struct {
    device_t     base;
    // Device block size, must be power of 2.
    uint64_t     block_size;
    // Number of blocks.
    uint64_t     block_count;
    // Device DMA alignment requirement; no more than block size.
    size_t       dma_align;
    // Native erased byte value.
    uint8_t      erase_value;
    // Block cache, if any.
    blk_cache_t *cache;
} device_block_t;

// Block device driver functions.
typedef struct {
    driver_t base;
    // Write device blocks.
    // The caller must ensure that `data` is aligned at least as much as needed for DMA.
    void (*write_blocks)(device_block_t *device, uint64_t start, uint64_t count, void const *data);
    // Read device blocks.
    // The caller must ensure that `data` is aligned at least as much as needed for DMA.
    void (*read_blocks)(device_block_t *device, uint64_t start, uint64_t count, void *data);
    // Test whether a single block is erased with native erase value.
    bool (*is_block_erased)(device_block_t *device, uint64_t start);
    // Erase blocks.
    void (*erase_blocks)(device_block_t *device, uint64_t start, uint64_t count, blkdev_erase_t mode);
    // [optional] Write device bytes.
    void (*write_bytes)(device_block_t *device, uint64_t start, uint64_t count, void const *data);
    // [optional] Read device bytes.
    void (*read_bytes)(device_block_t *device, uint64_t start, uint64_t count, void *data);
    // [optional] Erase bytes.
    void (*erase_bytes)(device_block_t *device, uint64_t start, uint64_t count, blkdev_erase_t mode);
} driver_block_t;



// Initialize a block device's cache.
// Number of lines must be a power of 2 at least 4.
// Optional for block devices with byte access methods.
bool device_block_init_cache(device_block_t *device, size_t num_lines);
// Clean up a block device's cache.
// Implicitly called by the device subsystem if a `device_block_t` is freed.
void device_block_free_cache(device_block_t *device);

// Write device blocks.
// The caller must ensure that `data` is aligned at least as much as needed for DMA.
bool device_block_write_blocks(device_block_t *device, uint64_t start, uint64_t count, void const *data);
// Read device blocks.
// The caller must ensure that `data` is aligned at least as much as needed for DMA.
bool device_block_read_blocks(device_block_t *device, uint64_t start, uint64_t count, void *data);
// Erase blocks.
bool device_block_erase_blocks(device_block_t *device, uint64_t start, uint64_t count, blkdev_erase_t mode);
// Write block device bytes.
// The alignment for DMA is handled by this function.
bool device_block_write_bytes(device_block_t *device, uint64_t offset, uint64_t size, void const *data);
// Read block device bytes.
// The alignment for DMA is handled by this function.
bool device_block_read_bytes(device_block_t *device, uint64_t offset, uint64_t size, void *data);
// Erase block device bytes.
bool device_block_erase_bytes(device_block_t *device, uint64_t offset, uint64_t size, blkdev_erase_t mode);

// Apply all pending changes.
bool device_block_sync_all(device_block_t *device);
// Apply pending changes in a range of blocks.
bool device_block_sync_blocks(device_block_t *device, uint64_t start, uint64_t count);
// Apply pending changes in a range of bytes.
bool device_block_sync_bytes(device_block_t *device, uint64_t offset, uint64_t size);
