
// SPDX-FileCopyrightText: 2025 Julian Scheffers <julian@scheffers.net>
// SPDX-FileType: SOURCE
// SPDX-License-Identifier: MIT

#pragma once

#include "cache.h"
#include "device/device.h"



// Block device erase mode.
typedef enum {
    // Erase with device's native erase value.
    BLKDEV_ERASE_NATIVE,
    // Erase and fill with zeroes.
    BLKDEV_ERASE_ZERO,
} blkdev_erase_t;

// Block device.
typedef struct {
    device_t base;
    // Device block size, must be power of 2.
    uint64_t block_size;
    // Number of blocks.
    uint64_t block_count;
    // Device DMA alignment requirement; no more than block size.
    size_t   dma_align;
    // Native erased byte value.
    uint8_t  erase_value;
    // Fast read; do not cache read data, only write data; requires byte read access.
    // If `false`, all accesses use entire blocks.
    bool     no_read_cache;
    // Do not cache at all; requires byte write/erase access.
    // If `false`, all accesses use entire blocks.
    bool     no_cache;
    // Block cache, if any.
    cache_t  cache;
} device_block_t;

// Block device driver functions.
typedef struct {
    driver_t base;
    // Write device blocks.
    // The caller must ensure that `data` is aligned at least as much as needed for DMA.
    errno_t (*write_blocks)(device_block_t *device, uint64_t start, uint64_t count, void const *data);
    // Read device blocks.
    // The caller must ensure that `data` is aligned at least as much as needed for DMA.
    errno_t (*read_blocks)(device_block_t *device, uint64_t start, uint64_t count, void *data);
    // Test whether a single block is erased with native erase value.
    errno_t (*is_block_erased)(device_block_t *device, uint64_t start);
    // Erase blocks.
    errno_t (*erase_blocks)(device_block_t *device, uint64_t start, uint64_t count, blkdev_erase_t mode);
    // [optional] Write device bytes.
    errno_t (*write_bytes)(device_block_t *device, uint64_t start, uint64_t count, void const *data);
    // [optional] Read device bytes.
    errno_t (*read_bytes)(device_block_t *device, uint64_t start, uint64_t count, void *data);
    // [optional] Erase bytes.
    errno_t (*erase_bytes)(device_block_t *device, uint64_t start, uint64_t count, blkdev_erase_t mode);
} driver_block_t;



// Initialize this block device's cache.
// Must be called by all block devices, during the `driver->add` function, if the device does not have `no_cache` set.
void device_block_init_cache(device_block_t *device);

// Write device blocks.
// The alignment for DMA is handled by this function.
errno_t device_block_write_blocks(device_block_t *device, uint64_t start, uint64_t count, void const *data);
// Read device blocks.
// The alignment for DMA is handled by this function.
errno_t device_block_read_blocks(device_block_t *device, uint64_t start, uint64_t count, void *data);
// Erase blocks.
errno_t device_block_erase_blocks(device_block_t *device, uint64_t start, uint64_t count, blkdev_erase_t mode);
// Write block device bytes.
// The alignment for DMA is handled by this function.
errno_t device_block_write_bytes(device_block_t *device, uint64_t offset, uint64_t size, void const *data);
// Read block device bytes.
// The alignment for DMA is handled by this function.
errno_t device_block_read_bytes(device_block_t *device, uint64_t offset, uint64_t size, void *data);
// Erase block device bytes.
errno_t device_block_erase_bytes(device_block_t *device, uint64_t offset, uint64_t size, blkdev_erase_t mode);

// Apply all pending changes.
// If `flush` is `true`, will remove the cache entries.
errno_t device_block_sync_all(device_block_t *device, bool flush);
// Apply pending changes in a range of blocks.
// If `flush` is `true`, will remove the cache entries.
errno_t device_block_sync_blocks(device_block_t *device, uint64_t start, uint64_t count, bool flush);
// Apply pending changes in a range of bytes.
// If `flush` is `true`, will remove the cache entries.
errno_t device_block_sync_bytes(device_block_t *device, uint64_t offset, uint64_t size, bool flush);
