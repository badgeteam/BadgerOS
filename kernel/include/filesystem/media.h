
// SPDX-License-Identifier: MIT

#pragma once

// Filesystem media types.
typedef enum {
    // Block device as media.
    FS_MEDIA_BLKDEV,
    // Linear area of RAM as media.
    FS_MEDIA_RAM,
    // Loopback file as media,
    // FS_MEDIA_FILE,
} fs_media_type_t;

// Filesystem media descriptor.
typedef struct fs_media fs_media_t;

#include "device/class/block.h"
#include "filesystem.h"

// Filesystem media descriptor.
struct fs_media {
    // Media type.
    fs_media_type_t type;
    // Partition offset added automatically.
    uint64_t        part_offset;
    // Partition length.
    uint64_t        part_length;
    union {
        // Block device handle.
        device_block_t *blkdev;
        // RAM area.
        uint8_t        *ram;
        // TODO: Loopback file support.
    };
};



// Read bytes from the media.
errno_t fs_media_read(fs_media_t const *media, uint64_t offset, size_t len, void *readbuf);
// Write bytes to the media.
errno_t fs_media_write(fs_media_t const *media, uint64_t offset, size_t len, void const *writebuf);
// Mark bytes on the media as erased.
errno_t fs_media_erase(fs_media_t const *media, uint64_t offset, size_t len);
// Sync bytes on the media to disk if cached.
// If `flush_read` is true, the read cache is also flushed here.
errno_t fs_media_sync(fs_media_t const *media, uint64_t offset, size_t len, bool flush_read);
// Sync all bytes on the media to disk if cached.
// If `flush_read` is true, the read cache is also flushed.
errno_t fs_media_sync_all(fs_media_t const *media, bool flush_read);
