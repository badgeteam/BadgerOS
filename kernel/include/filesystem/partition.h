
// SPDX-License-Identifier: MIT

#pragma once

#include "device/class/block.h"
#include "errno.h"
#include "malloc.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>



// Describes a single partition.
typedef struct {
    // On-disk byte offset.
    uint64_t offset;
    // On-disk byte size.
    uint64_t size;
    // Partition type.
    uint64_t type[2];
    // Partition UUID.
    uint64_t uuid[2];
    // Partition name.
    char    *name;
    // Partition name length.
    size_t   name_len;
    // Partition is read-only.
    bool     readonly;
} partition_t;

// Describes the partitioning system on a particular volume.
typedef struct {
    // Array of partitions.
    partition_t *parts;
    // Number of partitions.
    size_t       parts_len;
    // Volume label / name.
    char        *name;
    // Volume label length.
    size_t       name_len;
    // Disk UUID.
    uint64_t     uuid[2];
} volume_info_t;

// Return value of `get_volume_info`.
typedef struct {
    // Volume info.
    volume_info_t info;
    // Error code or boolean of whether `info` is valid.
    errno_bool_t  errno;
} get_volume_info_t;

// Get the volume information for a particular drive.
get_volume_info_t get_volume_info(device_block_t *device);

// Free a `partition_t`.
static inline void partition_free(partition_t part) {
    free(part.name);
}

// Free a `partition_list_t`.
static inline void volume_info_free(volume_info_t info) {
    for (size_t i = 0; i < info.parts_len; i++) {
        partition_free(info.parts[i]);
    }
    free(info.name);
}
