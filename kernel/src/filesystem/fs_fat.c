
// SPDX-License-Identifier: MIT

#include "filesystem/fs_fat.h"

#include "arrays.h"
#include "badge_err.h"
#include "filesystem.h"
#include "filesystem/fs_fat_types.h"
#include "filesystem/media.h"
#include "todo.h"

#define FATFS(vfs)    (*(fs_fat_t *)vfs->cookie)
#define FATFILE(file) (*(fs_fat_file_t *)file->cookie)

// TODO: Support for big-endian systems.



/* ==== FAT-specific functions ==== */

// Is this character legal in a FAT filename?
static inline bool is_legal_char(char c) {
    switch (c) {
        case 0 ... 0x1f:
        case '"':
        case '*':
        case '+':
        case ',':
        case '/':
        case ':':
        case ';':
        case '<':
        case '=':
        case '>':
        case '?':
        case '[':
        case '\\':
        case ']':
        case '|':
        case 0x7f ... 0xff: return false;
        default: return true;
    }
}

// Mangle a name into the 8.3 format used by FAT.
// Returns false if the name contains illegal characters.
static bool fat_mangle_name(char const *name, size_t name_len, char out[11], uint8_t *attr2_out, bool allow_shorten) {
    // Name may not start or end with spaces.
    if (!name_len || name[0] == ' ' || name[name_len - 1] == ' ') {
        return false;
    }
    for (size_t i = 0; i < name_len; i++) {
        if (!is_legal_char(name[i])) {
            return false;
        }
    }

    mem_set(out, ' ', 11);
    ptrdiff_t ext       = mem_last_index(name, name_len, '.');
    bool      shortened = false;

    // The extension.
    if (ext >= 0) {
        size_t max = name_len - ext - 1;
        if (max > 3) {
            max       = 3;
            shortened = true;
        }
        mem_copy(out + 8, name + ext + 1, max);
        name_len = ext;
    }

    // The remainder of the name.
    size_t max = name_len;
    if (shortened && max > 6) {
        max = 6;
    } else if (max > 8) {
        max       = 8;
        shortened = true;
    }
    mem_copy(out, name, max);

    // Add a ~1 at the end for shortened names.
    // If that collides when creating files, the number is incremented until it doesn't collide.
    if (shortened) {
        out[max]     = '~';
        out[max + 1] = '1';
    }

    int name_lc = 0;
    int ext_lc  = 0;

    // Convert to uppercase.
    for (size_t i = 0; i < 8; i++) {
        if (out[i] >= 'a' && out[i] <= 'z') {
            name_lc++;
            out[i] -= 'a' - 'A';
        }
    }
    for (size_t i = 8; i < 11; i++) {
        if (out[i] >= 'a' && out[i] <= 'z') {
            ext_lc++;
            out[i] -= 'a' - 'A';
        }
    }

    *attr2_out &= ~(FAT_ATTR2_LC_EXT | FAT_ATTR2_LC_NAME);
    if (name_lc >= 4) {
        *attr2_out |= FAT_ATTR2_LC_NAME;
    }
    if (ext_lc >= 2) {
        *attr2_out |= FAT_ATTR2_LC_EXT;
    }

    return allow_shorten || !shortened;
}

// Demangle the 8.3 format used by FAT into a name.
static void fat_demangle_name(char const mangled[11], uint8_t attr2, char *out, size_t *out_len) {
    size_t len = 0;

    // Add the name.
    for (size_t i = 0; i < 8; i++) {
        if (mangled[i] == ' ') {
            break;
        }
        char c = mangled[i];
        if ((attr2 & FAT_ATTR2_LC_NAME) && c >= 'A' && c <= 'Z') {
            c += 'a' - 'A';
        }
        out[len++] = c;
    }

    // Add a dot if there is an extension.
    if (mangled[8] != ' ') {
        out[len++] = '.';
        // Add the extension.
        for (size_t i = 8; i < 11; i++) {
            if (mangled[i] == ' ') {
                break;
            }
            char c = mangled[i];
            if ((attr2 & FAT_ATTR2_LC_EXT) && c >= 'A' && c <= 'Z') {
                c += 'a' - 'A';
            }
            out[len++] = c;
        }
    }

    *out_len = len;
}

// Read a FAT entry and translate into the 32-bit format.
static errno64_t read_fat_ent(vfs_t *vfs, uint32_t index) {
    // Upper 4 bits are ignored as per FAT spec.
    index &= 0x0fffffff;

    uint64_t base  = FATFS(vfs).fat_sector + FATFS(vfs).active_fat * FATFS(vfs).sectors_per_fat;
    base          *= FATFS(vfs).bytes_per_sector;
    switch (FATFS(vfs).type) {
        case FAT12: {
            mutex_acquire_shared(&FATFS(vfs).fat12_mutex, TIMESTAMP_US_MAX);
            // Read FAT12 entry.
            uint8_t raw[2];
            errno_t res = fs_media_read(vfs->media, base + index * 3 / 2, raw, 2);
            if (res < 0) {
                mutex_release_shared(&FATFS(vfs).fat12_mutex);
                return res;
            }

            // Convert into number.
            uint32_t ent;
            if (!(index & 1)) {
                ent = raw[0] | ((raw[1] & 0x0f) << 8);
            } else {
                ent = (raw[0] >> 4) | (raw[1] << 4);
            }
            // Adjust value.
            if (ent >= 0xff7) {
                ent |= 0x0ffff000;
            }
            mutex_release_shared(&FATFS(vfs).fat12_mutex);

            return ent;
        }

        case FAT16: {
            // Read FAT16 entry.
            uint8_t raw[2];
            errno_t res = fs_media_read(vfs->media, base + 2 * index, raw, 2);
            if (res < 0) {
                return res;
            }

            // Convert into number.
            uint32_t ent = raw[0] | (raw[1] << 8);
            // Adjust value.
            if (ent >= 0xfff7) {
                ent |= 0x0fff0000;
            }
            return ent;
        }

        case FAT32: {
            // Read FAT32 entry.
            uint8_t raw[4];
            errno_t res = fs_media_read(vfs->media, base + 4 * index, raw, 4);
            if (res < 0) {
                return res;
            }

            // Convert into number.
            uint32_t ent = raw[0] | (raw[1] << 8) | (raw[2] << 16) | (raw[3] << 24);
            return ent & 0x0fffffff;
        }

        default: __builtin_unreachable();
    }
}

// Write a FAT entry and translate into the filesystem's format.
static errno_t write_fat_ent(vfs_t *vfs, uint32_t index, uint32_t entry) {
    uint64_t base  = FATFS(vfs).fat_sector + FATFS(vfs).active_fat * FATFS(vfs).sectors_per_fat;
    base          *= FATFS(vfs).bytes_per_sector;
    switch (FATFS(vfs).type) {
        case FAT12: {
            mutex_acquire(&FATFS(vfs).fat12_mutex, TIMESTAMP_US_MAX);
            // Adjust value to fit into 12 bits.
            assert_dev_drop(!FS_FAT_IS_FAT_ALLOC(entry) || entry < 0x0ff7);
            entry &= 0x0fff;

            // Read existing data.
            uint8_t raw[2];
            errno_t res = fs_media_read(vfs->media, base + index * 3 / 2, raw, 2);
            if (res < 0) {
                mutex_release(&FATFS(vfs).fat12_mutex);
                return res;
            }

            // Modify current FAT entry.
            if (!(index & 1)) {
                raw[0] = entry & 0xff;
                raw[1] = (raw[1] & 0xf0) | ((entry >> 8) & 0x0f);
            } else {
                raw[0] = (raw[0] & 0x0f) | (entry << 4);
                raw[1] = entry >> 4;
            }

            // Write back.
            res = fs_media_write(vfs->media, base + index * 3 / 2, raw, 2);
            mutex_release(&FATFS(vfs).fat12_mutex);

            return res;
        } break;

        case FAT16: {
            // Adjust value to fit into 16 bits.
            assert_dev_drop(!FS_FAT_IS_FAT_ALLOC(entry) || entry < 0xfff7);
            entry &= 0xffff;

            // Convert to byte array.
            uint8_t raw[2] = {entry & 0xff, entry >> 8};

            // Write new entry.
            return fs_media_write(vfs->media, base + 2 * index, raw, 2);
        } break;

        case FAT32: {
            // Convert to byte array.
            uint8_t raw[4];
            errno_t res = fs_media_read(vfs->media, base + 4 * index, raw, 1);
            if (res < 0) {
                return res;
            }

            raw[0] = entry & 0xff;
            raw[1] = (entry >> 8) & 0xff;
            raw[2] = (entry >> 16) & 0xff;
            raw[3] = (raw[3] & 0xf0) | ((entry >> 24) & 0x0f);

            // Write new entry.
            return fs_media_write(vfs->media, base + 4 * index, raw, 4);
        } break;

        default: __builtin_unreachable();
    }
}

// Atomically allocate a single cluster for a file, but do not update the FAT.
static uint32_t alloc_cluster(vfs_t *vfs) {
    // Reserve one cluster.
    uint_fast32_t tmp;
    do {
        tmp = atomic_load_explicit(&FATFS(vfs).free_clusters, memory_order_relaxed);
        if (!tmp) {
            return 0;
        }
    } while (!atomic_compare_exchange_weak(&FATFS(vfs).free_clusters, &tmp, tmp - 1));

    // Find a cluster in the bitmap.
    for (size_t i = 0; i < (FATFS(vfs).cluster_count + sizeof(size_t) - 1) / sizeof(size_t); i++) {
        size_t tmp;
        size_t bit;
        while (1) {
            tmp = atomic_load_explicit(&FATFS(vfs).free_bitmap[i], memory_order_relaxed);
            if (!tmp) {
                // No free clusters here.
                break;
            }

            // Find first set bit; that is a free cluster.
#if SIZE_MAX == UINT32_MAX
            bit = __builtin_ctz(tmp);
#else
            bit = __builtin_ctzll(tmp);
#endif

            // Try to claim it.
            if (atomic_compare_exchange_weak(&FATFS(vfs).free_bitmap[i], &tmp, tmp & ~(1 << bit))) {
                // Success.
                return i * sizeof(size_t) * 8 + bit;
            }
        }
    }

    // Should never happen.
    logk(LOG_WARN, "Out of free clusters despite free_clusters being nonzero.");
    return 0;
}

// Free a single cluster, but do not update the FAT.
static void free_cluster(vfs_t *vfs, uint32_t cluster) {
    atomic_fetch_or(&FATFS(vfs).free_bitmap[cluster / sizeof(size_t)], 1 << (cluster % sizeof(size_t)));
    atomic_fetch_add(&FATFS(vfs).free_clusters, 1);
}

// Find a directory entry by name.
static errno_bool_t find_fat_dirent(
    vfs_t *vfs, vfs_file_obj_t *dir, char const *name, size_t name_len, fat_dirent_t *ent, uint32_t *ent_no_out
) {
    fileoff_t offset = 0;

    // TODO: LFN support.
    char    mangled[11];
    uint8_t attr2 = 0;
    if (!fat_mangle_name(name, name_len, mangled, &attr2, false)) {
        return -EINVAL;
    }

    while (offset < dir->size) {
        errno_t res = fs_fat_file_read(vfs, dir, offset, (void *)ent, sizeof(fat_dirent_t));
        if (res < 0) {
            return res;
        }
        if (ent->name[0] == 0) {
            return 0;
        }
        if (mem_equals(mangled, ent->name, 11)) {
            *ent_no_out = (uint32_t)offset / sizeof(fat_dirent_t);
            return 1;
        }
        offset += sizeof(fat_dirent_t);
    }

    return 0;
}

// Free a chain of clusters.
static errno_t free_cluster_chain(vfs_t *vfs, uint32_t cluster) {
    // Get block device parameters for erasing blocks.
    uint64_t bytes_per_cluster = FATFS(vfs).sectors_per_cluster * FATFS(vfs).bytes_per_sector;

    while (cluster != FS_FAT_FAT_EOF) {
        if (cluster < 2 || cluster >= FATFS(vfs).cluster_count) {
            logkf(LOG_ERROR, "Cluster %{u32;d} out of range (2-%{u32;d})", cluster, FATFS(vfs).cluster_count);
            return -EIO;
        }

        // Erase all blocks in the current cluster.
        errno_t res = fs_media_erase(vfs->media, (cluster - 2) * bytes_per_cluster, bytes_per_cluster);
        if (res < 0) {
            return res;
        }

        // Get next cluster index.
        errno64_t next = read_fat_ent(vfs, cluster);
        if (next < 0) {
            return next;
        }

        // Mark the cluster as free.
        res = write_fat_ent(vfs, cluster, 0);
        if (res < 0) {
            return res;
        }

        cluster = next;
    }

    return 0;
}

// Read the FAT starting at a certain cluster.
static errno_t read_cluster_chain(vfs_t *vfs, vfs_file_obj_t *file, uint32_t cluster) {
    while (true) {
        // Insert next cluster at the end of the array.
        if (!array_lencap_insert(
                &FATFILE(file).clusters,
                sizeof(uint32_t),
                &FATFILE(file).clusters_len,
                &FATFILE(file).clusters_cap,
                &cluster,
                FATFILE(file).clusters_len
            )) {
            // Out of memory.
            free(FATFILE(file).clusters);
            FATFILE(file).clusters     = NULL;
            FATFILE(file).clusters_len = 0;
            FATFILE(file).clusters_cap = 0;
            return -ENOMEM;
        }

        // Get the next cluster.
        errno64_t next = read_fat_ent(vfs, cluster);
        if (next < 0 || FS_FAT_IS_FAT_EOF(next)) {
            return next;
        }

        // Assert that cluster is not already in the chain.
        // TODO: This can probably be done faster.
        for (size_t i = 0; i < FATFILE(file).clusters_len; i++) {
            if (FATFILE(file).clusters[i] == next) {
                return -EIO;
            }
        }

        cluster = next;
    }

    return 0;
}



/* ==== VFS interface ==== */

// Try to mount a FAT filesystem.
errno_t fs_fat_mount(vfs_t *vfs) {
    // Do FAT filesystem sanity checks.
    if (!fs_fat_detect(vfs->media)) {
        return -EINVAL;
    }

    vfs->cookie = calloc(1, sizeof(fs_fat_t));
    if (!vfs->cookie) {
        return -ENOMEM;
    }

    fat_bpb_t bpb;
    errno_t   res = fs_media_read(vfs->media, 0, (void *)&bpb, sizeof(fat_bpb_t));
    if (res < 0) {
        free(vfs->cookie);
        return res;
    }
    fat32_header_t hdr32;
    fat16_header_t hdr16;

    if (!bpb.sector_count_32) {
        res = fs_media_read(vfs->media, sizeof(fat_bpb_t), (void *)&hdr32, sizeof(fat32_header_t));
        if (res < 0) {
            free(vfs->cookie);
            return res;
        }
        res = fs_media_read(
            vfs->media,
            sizeof(fat_bpb_t) + sizeof(fat32_header_t),
            (void *)&hdr16,
            sizeof(fat16_header_t)
        );
        if (res < 0) {
            free(vfs->cookie);
            return res;
        }
        FATFS(vfs).fat32_root_cluster = hdr32.first_root_cluster;
    } else {
        res = fs_media_read(vfs->media, sizeof(fat_bpb_t), (void *)&hdr16, sizeof(fat16_header_t));
        if (res < 0) {
            free(vfs->cookie);
            return res;
        }
    }

    // Calculate various sizes and positions.
    uint32_t total_sectors   = bpb.sector_count_16 ?: bpb.sector_count_32;
    size_t   sectors_per_fat = bpb.sectors_per_fat_16 ?: hdr32.sectors_per_fat_32;
    uint32_t root_sectors    = (bpb.root_entry_count * 32 + bpb.bytes_per_sector - 1) / bpb.bytes_per_sector;
    uint32_t data_sectors = total_sectors - bpb.reserved_sector_count - bpb.fat_count * sectors_per_fat - root_sectors;
    FATFS(vfs).sectors_per_fat     = sectors_per_fat;
    FATFS(vfs).cluster_count       = data_sectors / bpb.sectors_per_cluster;
    FATFS(vfs).fat_sector          = bpb.reserved_sector_count;
    FATFS(vfs).legacy_root_sector  = FATFS(vfs).fat_sector + sectors_per_fat * bpb.fat_count;
    FATFS(vfs).data_sector         = FATFS(vfs).legacy_root_sector + root_sectors;
    FATFS(vfs).bytes_per_sector    = bpb.bytes_per_sector;
    FATFS(vfs).legacy_root_size    = bpb.root_entry_count;
    FATFS(vfs).sectors_per_cluster = bpb.sectors_per_cluster;
    vfs->inode_root                = 1;

    // Determine filesystem type.
    if (FATFS(vfs).cluster_count < 4085) {
        FATFS(vfs).type = FAT12;
        mutex_init(&FATFS(vfs).fat12_mutex, true);
    } else if (FATFS(vfs).cluster_count < 65525) {
        FATFS(vfs).type = FAT16;
    } else {
        FATFS(vfs).type = FAT32;
    }

    // Read the FAT and populate the FAT usage bitmap.
    size_t n_bitmap_ents   = (FATFS(vfs).cluster_count + sizeof(size_t) - 1) / sizeof(size_t);
    FATFS(vfs).free_bitmap = calloc(1, n_bitmap_ents * sizeof(size_t));
    if (!FATFS(vfs).free_bitmap) {
        free(vfs->cookie);
        return -ENOMEM;
    }
    for (uint32_t i = 0; i < FATFS(vfs).cluster_count; i++) {
        errno64_t ent = read_fat_ent(vfs, i);
        if (ent < 0) {
            free(FATFS(vfs).free_bitmap);
            free(vfs->cookie);
            return ent;
        }
        if (FS_FAT_IS_FAT_FREE(ent)) {
            size_t idx                   = i / sizeof(size_t);
            size_t bit                   = i % sizeof(size_t);
            FATFS(vfs).free_bitmap[idx] |= 1 << bit;
            FATFS(vfs).free_clusters++;
        }
    }

    // Mark clusters 0 and 1 as used.
    FATFS(vfs).free_bitmap[0] &= ~3;

    atomic_thread_fence(memory_order_release);

    return 0;
}

// Unmount a FAT filesystem.
void fs_fat_umount(vfs_t *vfs) {
    // TODO.
    free(FATFS(vfs).free_bitmap);
    free(vfs->cookie);
}

// Identify whether a block device contains a FAT filesystem.
// Returns false on error.
errno_bool_t fs_fat_detect(fs_media_t *dev) {
    // Read BPB.
    fat_bpb_t bpb;
    errno_t   res = fs_media_read(dev, 0, (void *)&bpb, sizeof(fat_bpb_t));
    if (res < 0) {
        return res;
    }

    // Checked signature #1: valid sector size.
    // Sector size is an integer power of 2 from 512 to 4096.
    if (bpb.bytes_per_sector < 512 || bpb.bytes_per_sector > 4096) {
        return false;
    }
    if (bpb.bytes_per_sector & (bpb.bytes_per_sector - 1)) {
        return false;
    }

    // Checked signature #2: FAT BPB signature bytes.
    uint8_t tmp[2];
    res = fs_media_read(dev, 510, tmp, sizeof(tmp));
    if (res < 0) {
        return res;
    }
    if (tmp[0] != 0x55 || tmp[1] != 0xAA) {
        return false;
    }

    // Both minor signature checks passed, this filesystem is probably FAT.
    return true;
}



// Allocate a dirent for file creation.
// TODO: Allocate multiple for LFN support.
static errno64_t alloc_dirent(vfs_t *vfs, vfs_file_obj_t *dir) {
    // Find a free dirent.
    for (uint32_t i = 0; i < dir->size / sizeof(fat_dirent_t); i++) {
        uint8_t tmp;
        errno_t res = fs_fat_file_read(vfs, dir, i * sizeof(fat_dirent_t) + offsetof(fat_dirent_t, name), &tmp, 1);
        if (res < 0) {
            return res;
        }
        if (tmp == 0 || tmp == 0xe5) {
            return i;
        }
    }

    if (dir->is_vfs_root && FATFS(vfs).type != FAT32) {
        // FAT12 and FAT16 root dirs are not resizable.
        return -ENOSPC;
    }

    // Allocate an additional cluster for the directory.
    uint32_t cluster = alloc_cluster(vfs);
    if (!cluster) {
        return -ENOSPC;
    }

    // Add to cluster chain.
    if (!array_lencap_insert(
            &FATFILE(dir).clusters,
            sizeof(uint32_t),
            &FATFILE(dir).clusters_len,
            &FATFILE(dir).clusters_cap,
            &cluster,
            FATFILE(dir).clusters_len
        )) {
        free_cluster(vfs, cluster);
        return -ENOMEM;
    }

    // Zero out the new cluster.
    // Only the first byte needs to be cleared; the rest will then be ignored.
    uint8_t zero = 0;
    errno_t res  = fs_media_write(
        vfs->media,
        (FATFS(vfs).data_sector + (cluster - 2) * FATFS(vfs).sectors_per_cluster) * FATFS(vfs).bytes_per_sector,
        &zero,
        1
    );
    if (res < 0) {
        FATFILE(dir).clusters_len--;
        free_cluster(vfs, cluster);
        return res;
    }

    // Write the new FAT entries.
    res = write_fat_ent(vfs, FATFILE(dir).clusters[FATFILE(dir).clusters_len - 1], FS_FAT_FAT_EOF);
    if (res < 0) {
        FATFILE(dir).clusters_len--;
        free_cluster(vfs, cluster);
        return res;
    }
    if (FATFILE(dir).clusters_len > 1) {
        res = write_fat_ent(vfs, FATFILE(dir).clusters[FATFILE(dir).clusters_len - 2], cluster);
        if (res < 0) {
            FATFILE(dir).clusters_len--;
            free_cluster(vfs, cluster);
            return res;
        }
    }

    // Update parent dirent.
    if (!dir->size) {
        assert_dev_drop(!dir->is_vfs_root);
        // Update first cluster field in parent dirent.
        uint8_t buf[] = {
            FATFILE(dir).clusters[0] >> 0,
            FATFILE(dir).clusters[0] >> 8,
            FATFILE(dir).clusters[0] >> 16,
            FATFILE(dir).clusters[0] >> 24,
        };

        res = fs_fat_file_write(
            vfs,
            FATFILE(dir).parent,
            FATFILE(dir).dirent_no * sizeof(fat_dirent_t) + offsetof(fat_dirent_t, first_cluster_lo),
            buf,
            2
        );
        if (res < 0) {
            return res;
        }

        res = fs_fat_file_write(
            vfs,
            FATFILE(dir).parent,
            FATFILE(dir).dirent_no * sizeof(fat_dirent_t) + offsetof(fat_dirent_t, first_cluster_hi),
            buf + 2,
            2
        );
        if (res < 0) {
            return res;
        }
    }

    uint32_t ent_no  = dir->size / sizeof(fat_dirent_t);
    dir->size       += FATFS(vfs).sectors_per_cluster * FATFS(vfs).bytes_per_sector;

    return ent_no;
}

// Create a new file or directory.
static errno_t create_file_impl(vfs_t *vfs, vfs_file_obj_t *dir, char const *name, size_t name_len, bool is_dir) {
    // Format entry.
    // TODO: BadgerOS doesn't have dates yet, so ctime cannot be set.
    fat_dirent_t ent = {0};
    if (!fat_mangle_name(name, name_len, ent.name, &ent.attr2, true)) {
        return -EINVAL;
    }
    ent.attr = is_dir ? FAT_ATTR_DIRECTORY : 0;

    // Allocate a cluster for the new directory.
    uint32_t cluster;
    if (is_dir) {
        cluster = alloc_cluster(vfs);
        if (cluster == (uint32_t)-1) {
            return -ENOSPC;
        }
        ent.first_cluster_lo = cluster;
        ent.first_cluster_hi = cluster >> 16;
    }

    // Try to alloc space for this dirent.
    uint32_t index = alloc_dirent(vfs, dir);
    if (index == (uint32_t)-1) {
        if (is_dir) {
            free_cluster(vfs, cluster);
        }
        return -ENOSPC;
    }

    // Write the new dirent.
    uint8_t cur;
    errno_t res = fs_fat_file_read(vfs, dir, index * sizeof(fat_dirent_t), &cur, 1);
    if (res < 0) {
        if (is_dir) {
            free_cluster(vfs, cluster);
        }
        return res;
    }
    res = fs_fat_file_write(vfs, dir, index * sizeof(fat_dirent_t), (void *)&ent, sizeof(fat_dirent_t));
    if (res < 0) {
        if (is_dir) {
            free_cluster(vfs, cluster);
        }
        return res;
    }

    // Preserve NULL terminating entry.
    uint8_t const zero = 0;
    if (cur == 0 && (fileoff_t)((index + 1) * sizeof(fat_dirent_t)) < dir->size) {
        res = fs_fat_file_write(vfs, dir, (index + 1) * sizeof(fat_dirent_t), &zero, 1);
        if (res < 0) {
            return res;
        }
    }

    if (is_dir) {
        res = write_fat_ent(vfs, cluster, FS_FAT_FAT_EOF);
        if (res < 0) {
            return res;
        }
        uint64_t bytes_per_clus = FATFS(vfs).bytes_per_sector * FATFS(vfs).sectors_per_cluster;
        uint64_t data_off       = FATFS(vfs).data_sector * FATFS(vfs).bytes_per_sector;

        // Write . entry.
        fat_dirent_t ent2 = {0};
        mem_set(ent2.name, ' ', sizeof(ent2.name));
        ent2.name[0]          = '.';
        ent2.attr             = FAT_ATTR_DIRECTORY;
        ent2.first_cluster_lo = cluster;
        ent2.first_cluster_hi = cluster >> 16;
        res =
            fs_media_write(vfs->media, (cluster - 2) * bytes_per_clus + data_off, (void *)&ent2, sizeof(fat_dirent_t));
        if (res < 0) {
            return res;
        }

        // Write .. entry.
        uint32_t parent_cluster = 0;
        if (!dir->is_vfs_root) {
            parent_cluster = FATFILE(dir).clusters[0];
        }
        ent2.name[1]          = '.';
        ent2.attr             = FAT_ATTR_DIRECTORY;
        ent2.first_cluster_lo = parent_cluster;
        ent2.first_cluster_hi = parent_cluster >> 16;
        res                   = fs_media_write(
            vfs->media,
            (cluster - 2) * bytes_per_clus + data_off + sizeof(fat_dirent_t),
            (void *)&ent2,
            sizeof(fat_dirent_t)
        );
        if (res < 0) {
            return res;
        }

        // Write the NULL terminating entry.
        return fs_media_write(
            vfs->media,
            (cluster - 2) * bytes_per_clus + data_off + 2 * sizeof(fat_dirent_t),
            &zero,
            1
        );
    }

    return 0;
}

// Insert a new file into the given directory.
errno_t fs_fat_create_file(vfs_t *vfs, vfs_file_obj_t *dir, char const *name, size_t name_len) {
    return create_file_impl(vfs, dir, name, name_len, false);
}

// Insert a new directory into the given directory.
errno_t fs_fat_create_dir(vfs_t *vfs, vfs_file_obj_t *dir, char const *name, size_t name_len) {
    return create_file_impl(vfs, dir, name, name_len, true);
}

// Helper that checks the directory (except first two dirents) is empty.
static errno_bool_t is_dir_empty(vfs_t *vfs, uint32_t _cluster) {
    errno64_t cluster        = _cluster;
    uint32_t  ent            = 2;
    uint64_t  bytes_per_clus = FATFS(vfs).bytes_per_sector * FATFS(vfs).sectors_per_cluster;
    uint32_t  ents_per_clus  = bytes_per_clus / sizeof(fat_dirent_t);

    while (cluster != FS_FAT_FAT_EOF) {
        // `ent` is not initialized in the loop so that, for the first cluster only, the first two dirents are skipped.
        for (; ent < ents_per_clus; ent++) {
            uint8_t data;
            errno_t res =
                fs_media_read(vfs->media, (cluster - 2) * bytes_per_clus + ent * sizeof(fat_dirent_t), &data, 1);
            if (res < 0) {
                return res;
            }

            if (data == 0) {
                return true;
            } else if (data != 0xE5) {
                return false;
            }
        }
        ent = 0;

        // Go to the next cluster.
        cluster = read_fat_ent(vfs, cluster);
        if (cluster < 0) {
            return cluster;
        }
    }

    return true;
}

// Unlink a file from the given directory.
// If the file is currently open, the file object for it is provided in `file`.
errno_t fs_fat_unlink(vfs_t *vfs, vfs_file_obj_t *dir, char const *name, size_t name_len, vfs_file_obj_t *file) {
    // Check for attempting to rmdir `.` or `..`.
    if ((name_len == 1 && name[0] == '.') || (name_len == 2 && name[0] == '.' && name[1] == '.')) {
        return -EINVAL;
    }

    // Find dirent to remove.
    fat_dirent_t ent;
    uint32_t     ent_no;
    errno_bool_t found = find_fat_dirent(vfs, dir, name, name_len, &ent, &ent_no);
    if (found != 1) {
        return found ?: -ENOENT;
    }
    uint32_t first_cluster = (ent.first_cluster_hi << 16) | ent.first_cluster_lo;

    // Check for attempting to rmdir the root directory from elsewhere.
    if ((ent.attr & FAT_ATTR_DIRECTORY) && first_cluster == 0) {
        return -EIO;
    }

    // Assert the directory to be empty apart from the . and .. entries.
    if (ent.attr & FAT_ATTR_DIRECTORY) {
        errno_bool_t empty = is_dir_empty(vfs, first_cluster);
        if (empty != 1) {
            return empty ?: -ENOTEMPTY;
        }
    }

    // Actually remove the dirent.
    if (file) {
        file->links = 0;
    } else {
        free_cluster_chain(vfs, first_cluster);
    }
    uint8_t data = 0xe9;
    return fs_fat_file_write(vfs, dir, sizeof(fat_dirent_t) * ent_no, &data, 1);
}

// FAT doesn't support this; returns -ENOTSUP.
errno_t fs_fat_link(
    vfs_t *vfs, vfs_file_obj_t *old_obj, vfs_file_obj_t *new_dir, char const *new_name, size_t new_name_len
) {
    (void)vfs;
    (void)old_obj;
    (void)new_dir;
    (void)new_name;
    (void)new_name_len;
    return -ENOTSUP;
}

// FAT doesn't support this; returns -ENOTSUP.
errno_t fs_fat_symlink(
    vfs_t          *vfs,
    char const     *target_path,
    size_t          target_path_len,
    vfs_file_obj_t *link_dir,
    char const     *link_name,
    size_t          link_name_len
) {
    (void)vfs;
    (void)target_path;
    (void)target_path_len;
    (void)link_dir;
    (void)link_name;
    (void)link_name_len;
    return -ENOTSUP;
}

// FAT doesn't support this; returns -ENOTSUP.
errno_t fs_fat_mkfifo(vfs_t *vfs, vfs_file_obj_t *dir, char const *name, size_t name_len) {
    (void)vfs;
    (void)dir;
    (void)name;
    (void)name_len;
    return -ENOTSUP;
}


// Convert a FAT dirent to a generic dirent.
static void fat_dirent_conv(fat_dirent_t const *fat_ent, dirent_t *ent) {
    size_t name_len_tmp;
    fat_demangle_name(fat_ent->name, fat_ent->attr2, ent->name, &name_len_tmp);
    ent->name_len   = name_len_tmp;
    ent->inode      = fat_ent->first_cluster_lo | (fat_ent->first_cluster_hi << 16);
    ent->is_dir     = fat_ent->attr & FAT_ATTR_DIRECTORY;
    ent->is_symlink = false;
    ent->record_len = offsetof(dirent_t, name) + ent->name_len + 1;
}

// Read all entries from a directory.
errno_dirent_list_t fs_fat_dir_read(vfs_t *vfs, vfs_file_obj_t *dir) {
    TODO();
}

// Read the directory entry with the matching name.
// Returns true if the entry was found.
errno_bool_t fs_fat_dir_find_ent(vfs_t *vfs, vfs_file_obj_t *dir, dirent_t *ent, char const *name, size_t name_len) {
    // Edge case: `..` at root; this also means that the root FS is *this* FAT filesystem.
    // `.` at any directory is handled by the VFS layer.
    // Due to how VFS caches file objects, this removes the need supporting `..` at root in fs_fat_file_open.
    if (name_len == 2 && name[0] == '.' && name[1] == '.' && dir->is_vfs_root) {
        ent->inode      = 1;
        ent->is_dir     = true;
        ent->is_symlink = false;
        ent->name_len   = 2;
        ent->record_len = offsetof(dirent_t, name) + 3;
        mem_copy(ent->name, "..", 3);
        return true;
    }

    // Read FAT-specific directory entry.
    fat_dirent_t fat_ent;
    uint32_t     fat_ent_no;
    errno_bool_t res = find_fat_dirent(vfs, dir, name, name_len, &fat_ent, &fat_ent_no);
    if (res != 1) {
        return res;
    }

    // Convert to generic VFS dirent.
    fat_dirent_conv(&fat_ent, ent);
    return true;
}



// Stat a file object.
errno_t fs_fat_stat(vfs_t *vfs, vfs_file_obj_t *file, stat_t *stat) {
    TODO();
}



// Open a file handle for the root directory.
errno_t fs_fat_root_open(vfs_t *vfs, vfs_file_obj_t *file) {
    file->is_vfs_root = true;
    file->type        = FILETYPE_DIR;
    file->inode       = 1;
    file->is_vfs_root = true;
    file->cookie      = calloc(1, sizeof(fs_fat_file_t));
    if (!file->cookie) {
        return -ENOMEM;
    }

    if (FATFS(vfs).type == FAT32) {
        // FAT32 root dir length is determined by the length of the chain in the FAT.
        errno_t res = read_cluster_chain(vfs, file, FATFS(vfs).fat32_root_cluster);
        if (res < 0) {
            return res;
        }
        file->size = FATFILE(file).clusters_len * FATFS(vfs).sectors_per_cluster * FATFS(vfs).bytes_per_sector;
    } else {
        // FAT16/FAT12 root dir length is determined by the BPB.
        file->size = FATFS(vfs).legacy_root_size * sizeof(fat_dirent_t);
    }

    return 0;
}

// Open a file for reading and/or writing.
errno_t fs_fat_file_open(vfs_t *vfs, vfs_file_obj_t *dir, vfs_file_obj_t *file, char const *name, size_t name_len) {
    // Look up dirent.
    fat_dirent_t ent;
    errno_bool_t found = find_fat_dirent(vfs, dir, name, name_len, &ent, &FATFILE(file).dirent_no);
    if (found != 1) {
        return found ?: -ENOENT;
    }

    file->cookie = calloc(1, sizeof(fs_fat_file_t));
    if (!file->cookie) {
        return -ENOMEM;
    }

    // Get on-disk position of dirent; this will serve as the inode number of files and non-root dirs.
    uint64_t dirent_pos;
    if (FATFS(vfs).type == FAT32) {
        uint64_t dirents_per_clus = FATFS(vfs).bytes_per_sector * FATFS(vfs).sectors_per_cluster / sizeof(fat_dirent_t);

        dirent_pos = FATFILE(dir).clusters[FATFILE(file).dirent_no / dirents_per_clus] +
                     FATFILE(file).dirent_no * sizeof(fat_dirent_t) +
                     FATFS(vfs).data_sector * FATFS(vfs).bytes_per_sector;
    } else {
        dirent_pos = FATFS(vfs).legacy_root_sector * FATFS(vfs).bytes_per_sector +
                     FATFILE(file).dirent_no * sizeof(fat_dirent_t);
    }

    // Enter FAT infomation into file object.
    file->inode            = dirent_pos / sizeof(fat_dirent_t);
    file->type             = ent.attr & FAT_ATTR_DIRECTORY ? FILETYPE_DIR : FILETYPE_FILE;
    file->is_vfs_root      = false;
    file->links            = 1;
    uint32_t first_cluster = ent.first_cluster_lo | (ent.first_cluster_hi << 16);
    if (first_cluster) {
        errno_t res = read_cluster_chain(vfs, file, first_cluster);
        if (res < 0) {
            free(file->cookie);
            return res;
        }
    }
    vfs_file_push_ref(dir);
    FATFILE(file).parent = dir;

    // File size differs between files and directories.
    if (file->type == FILETYPE_DIR) {
        file->size = FATFS(vfs).sectors_per_cluster * FATFS(vfs).bytes_per_sector * FATFILE(file).clusters_len;
    } else {
        file->size = ent.size;
    }

    return 0;
}

// Close a file opened by `fs_fat_file_open`.
// Only raises an error if `file` is an invalid file descriptor.
errno_t fs_fat_file_close(vfs_t *vfs, vfs_file_obj_t *file) {
    errno_t res = 0;
    if (file->links == 0 && FATFILE(file).clusters_len) {
        res = free_cluster_chain(vfs, FATFILE(file).clusters[0]);
    }
    free(FATFILE(file).clusters);
    free(file->cookie);
    return res;
}

// Read bytes from a file.
errno_t fs_fat_file_read(vfs_t *vfs, vfs_file_obj_t *file, fileoff_t offset, uint8_t *readbuf, fileoff_t readlen) {
    // Bounds check.
    if (offset < 0 || readlen < 0 || offset + readlen > file->size) {
        return -EIO;
    }

    if (file->is_vfs_root && FATFS(vfs).type != FAT32) {
        // Special case: FAT12/FAT16 root directory.
        return fs_media_read(
            vfs->media,
            FATFS(vfs).legacy_root_sector * FATFS(vfs).bytes_per_sector + offset,
            readbuf,
            readlen
        );
    }

    // Normal files and FAT32 root directory.
    uint32_t bytes_per_cluster = FATFS(vfs).sectors_per_cluster * FATFS(vfs).bytes_per_sector;
    while (readlen) {
        // Find the cluster containing the offset.
        uint32_t cluster_offset = offset / bytes_per_cluster;
        if (cluster_offset >= FATFILE(file).clusters_len) {
            // Error: File should have more clusters.
            return -EIO;
        }
        uint32_t cluster = FATFILE(file).clusters[cluster_offset] & 0x0fffffff;

        // Read from this cluster.
        uint32_t max = bytes_per_cluster - offset % bytes_per_cluster;
        if (max > readlen) {
            max = readlen;
        }
        errno_t res = fs_media_read(
            vfs->media,
            FATFS(vfs).data_sector * FATFS(vfs).bytes_per_sector + (cluster - 2) * bytes_per_cluster +
                offset % bytes_per_cluster,
            readbuf,
            max
        );
        if (res < 0) {
            return res;
        }

        // Move to the next cluster.
        readbuf += max;
        readlen -= max;
        offset  += max;
    }

    return 0;
}

// Write without bounds check.
static errno_t fs_fat_file_write_unsafe(
    vfs_t *vfs, vfs_file_obj_t *file, fileoff_t offset, uint8_t const *writebuf, fileoff_t writelen
) {
    if (file->is_vfs_root && FATFS(vfs).type != FAT32) {
        // Special case: FAT12/FAT16 root directory.
        return fs_media_write(
            vfs->media,
            FATFS(vfs).legacy_root_sector * FATFS(vfs).bytes_per_sector + offset,
            writebuf,
            writelen
        );
    }

    // Normal files and FAT32 root directory.
    uint32_t bytes_per_cluster = FATFS(vfs).sectors_per_cluster * FATFS(vfs).bytes_per_sector;
    while (writelen) {
        // Find the cluster containing the offset.
        uint32_t cluster_offset = offset / bytes_per_cluster;
        if (cluster_offset >= FATFILE(file).clusters_len) {
            // Error: File should have more clusters.
            return -EIO;
        }
        uint32_t cluster = FATFILE(file).clusters[cluster_offset] & 0x0fffffff;

        // Write to this cluster.
        uint32_t max = bytes_per_cluster - offset % bytes_per_cluster;
        if (max > writelen) {
            max = writelen;
        }
        errno_t res = fs_media_write(
            vfs->media,
            FATFS(vfs).data_sector * FATFS(vfs).bytes_per_sector + (cluster - 2) * bytes_per_cluster +
                offset % bytes_per_cluster,
            writebuf,
            max
        );
        if (res < 0) {
            return res;
        }

        // Move to the next cluster.
        writebuf += max;
        writelen -= max;
        offset   += max;
    }

    return 0;
}

// Write bytes from a file.
errno_t
    fs_fat_file_write(vfs_t *vfs, vfs_file_obj_t *file, fileoff_t offset, uint8_t const *writebuf, fileoff_t writelen) {
    // Bounds check.
    if (offset < 0 || writelen < 0 || offset + writelen > file->size) {
        return -EIO;
    }
    return fs_fat_file_write_unsafe(vfs, file, offset, writebuf, writelen);
}

// Change the length of a file opened by `fs_fat_file_open`.
errno_t fs_fat_file_resize(vfs_t *vfs, vfs_file_obj_t *file, fileoff_t new_size) {
    // Bounds check.
    if (new_size < 0) {
        return -EIO;
    }

    // Calculate the new number of clusters.
    uint32_t new_clusters = (new_size + FATFS(vfs).sectors_per_cluster * FATFS(vfs).bytes_per_sector - 1) /
                            (FATFS(vfs).sectors_per_cluster * FATFS(vfs).bytes_per_sector);
    uint32_t old_clusters = FATFILE(file).clusters_len;

    if (new_clusters > FATFILE(file).clusters_len) {
        // Allocate new clusters.
        for (uint32_t i = old_clusters; i < new_clusters; i++) {
            uint32_t cluster = alloc_cluster(vfs);
            if (!cluster) {
                // Allocation failed.
                return -ENOSPC;
            }

            if (!array_lencap_insert(
                    &FATFILE(file).clusters,
                    sizeof(uint32_t),
                    &FATFILE(file).clusters_len,
                    &FATFILE(file).clusters_cap,
                    &cluster,
                    i
                )) {
                // Out of memory.
                free_cluster(vfs, cluster);
                return -ENOMEM;
            }

            // Update prev. FAT entry to point to current.
            if (i) {
                errno_t res = write_fat_ent(vfs, FATFILE(file).clusters[i - 1], cluster);
                if (res < 0) {
                    return res;
                }
            }
        }
    } else if (new_clusters < FATFILE(file).clusters_len) {
        // Free excess clusters.
        for (uint32_t i = new_clusters; i < FATFILE(file).clusters_len; i++) {
            // Mark as free on disk.
            errno_t res = write_fat_ent(vfs, FATFILE(file).clusters[i], 0);
            if (res < 0) {
                return res;
            }

            // Mark as free in memory.
            free_cluster(vfs, FATFILE(file).clusters[i]);
        }
    }

    if (new_size > file->size) {
        // Fill new space with zeroes.
        fileoff_t pos        = file->size;
        fileoff_t zeroes_len = 4096; // TODO: Replace with one central page-aligned 4K of zeroes.
        void     *zeroes     = calloc(1, zeroes_len);
        while (pos < new_size) {
            fileoff_t max = pos + zeroes_len > new_size ? new_size - pos : zeroes_len;
            errno_t   res = fs_fat_file_write_unsafe(vfs, file, pos, zeroes, max);
            if (res < 0) {
                free(zeroes);
                return res;
            }
            pos += max;
        }
        free(zeroes);
    }

    if (new_clusters != old_clusters) {
        // Update last FAT entry.
        errno_t res = write_fat_ent(vfs, FATFILE(file).clusters[new_clusters - 1], FS_FAT_FAT_EOF);
        if (res < 0) {
            return res;
        }
    }

    // Update dirent.
    uint8_t buf[4] = {new_size, new_size >> 8, new_size >> 16, new_size >> 24};
    errno_t res    = fs_fat_file_write(
        vfs,
        FATFILE(file).parent,
        FATFILE(file).dirent_no * sizeof(fat_dirent_t) + offsetof(fat_dirent_t, size),
        buf,
        4
    );
    if (res < 0) {
        return res;
    }

    if (!new_clusters && old_clusters) {
        // If new clusters is 0, the first cluster no. must be removed from the dirent.
        mem_set(buf, 0, sizeof(buf));

    } else if (!old_clusters && new_clusters) {
        // The opposite applies if there used to be no clusters.
        buf[0] = FATFILE(file).clusters[0];
        buf[1] = FATFILE(file).clusters[0] >> 8;
        buf[2] = FATFILE(file).clusters[0] >> 16;
        buf[3] = FATFILE(file).clusters[0] >> 24;
    }

    if ((file->links && !new_clusters && old_clusters) || (!old_clusters && new_clusters)) {
        // Update first cluster field unless the file is unlinked (it would have no dirent).
        res = fs_fat_file_write(
            vfs,
            FATFILE(file).parent,
            FATFILE(file).dirent_no * sizeof(fat_dirent_t) + offsetof(fat_dirent_t, first_cluster_lo),
            buf,
            2
        );
        if (res < 0) {
            return res;
        }

        res = fs_fat_file_write(
            vfs,
            FATFILE(file).parent,
            FATFILE(file).dirent_no * sizeof(fat_dirent_t) + offsetof(fat_dirent_t, first_cluster_hi),
            buf + 2,
            2
        );
        if (res < 0) {
            return res;
        }
    }

    // Update the file size.
    file->size = new_size;
    return 0;
}



// Commit all pending writes to disk.
// The filesystem, if it does caching, must always sync everything to disk at once.
errno_t fs_fat_flush(vfs_t *vfs) {
    TODO();
}



static vfs_vtable_t fs_fat_vtable = {
    .mount        = fs_fat_mount,
    .umount       = fs_fat_umount,
    .create_file  = fs_fat_create_file,
    .create_dir   = fs_fat_create_dir,
    .unlink       = fs_fat_unlink,
    .rmdir        = fs_fat_unlink,
    .link         = fs_fat_link,
    .symlink      = fs_fat_symlink,
    .mkfifo       = fs_fat_mkfifo,
    .dir_read     = fs_fat_dir_read,
    .dir_find_ent = fs_fat_dir_find_ent,
    .stat         = fs_fat_stat,
    .root_open    = fs_fat_root_open,
    .file_open    = fs_fat_file_open,
    .file_close   = fs_fat_file_close,
    .file_read    = fs_fat_file_read,
    .file_write   = fs_fat_file_write,
    .file_resize  = fs_fat_file_resize,
    // .file_flush   = fs_fat_file_flush,
    .flush        = fs_fat_flush,
};

FS_DRIVER_DECL(fs_fat_driver) = {
    .detect = fs_fat_detect,
    .id     = "vfat",
    .vtable = &fs_fat_vtable,
};
