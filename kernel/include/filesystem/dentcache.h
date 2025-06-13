
// SPDX-License-Identifier: MIT

#pragma once

#include "filesystem.h"
#include "map.h"

// Dirent cache entry.
typedef struct dentcache dentcache_t;

// Dirent cache entry.
struct dentcache {
    // Parent dirent.
    dentcache_t *parent;
    // Map of child entries.
    map_t        children;
    // This dirent.
    dirent_t     dirent;
};
