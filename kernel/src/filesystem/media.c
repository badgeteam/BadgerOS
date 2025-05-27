
// SPDX-License-Identifier: MIT

#include "filesystem/media.h"

#include "device/class/block.h"
#include "log.h"
#include "todo.h"

#define prelude(name)                                                                                                  \
    if (offset + len < offset || offset + len > media->part_length) {                                                  \
        logk(LOG_ERROR, "Out-of-bounds media " #name " ignored");                                                      \
        return -EIO;                                                                                                   \
    }                                                                                                                  \
    offset += media->part_offset;



// Read bytes from the media.
errno_t fs_media_read(fs_media_t const *media, uint64_t offset, void *readbuf, size_t len) {
    prelude(read);
    switch (media->type) {
        case FS_MEDIA_BLKDEV: return device_block_read_bytes(media->blkdev, offset, len, readbuf);
        case FS_MEDIA_RAM: mem_copy(readbuf, media->ram + offset, len); return 0;
        default: TODO();
    }
}

// Write bytes to the media.
errno_t fs_media_write(fs_media_t const *media, uint64_t offset, void const *writebuf, size_t len) {
    prelude(write);
    switch (media->type) {
        case FS_MEDIA_BLKDEV: return device_block_write_bytes(media->blkdev, offset, len, writebuf);
        case FS_MEDIA_RAM: mem_copy(media->ram + offset, writebuf, len); return 0;
        default: TODO();
    }
}

// Mark bytes on the media as erased.
errno_t fs_media_erase(fs_media_t const *media, uint64_t offset, size_t len) {
    prelude(erase);
    switch (media->type) {
        case FS_MEDIA_BLKDEV: return device_block_erase_bytes(media->blkdev, offset, len, BLKDEV_ERASE_NATIVE);
        case FS_MEDIA_RAM: return 0;
        default: TODO();
    }
}

// Sync bytes on the media to disk if cached.
// If `flush_read` is true, the read cache is also flushed here.
errno_t fs_media_sync(fs_media_t const *media, uint64_t offset, size_t len, bool flush_read) {
    prelude(sync);
    switch (media->type) {
        case FS_MEDIA_BLKDEV: return device_block_sync_bytes(media->blkdev, offset, len, flush_read);
        case FS_MEDIA_RAM: return 0;
        default: TODO();
    }
}

// Sync all bytes on the media to disk if cached.
// If `flush_read` is true, the read cache is also flushed.
errno_t fs_media_sync_all(fs_media_t const *media, bool flush_read) {
    return fs_media_sync(media, 0, media->part_length, flush_read);
}
