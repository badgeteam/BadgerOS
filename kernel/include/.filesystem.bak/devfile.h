
// SPDX-License-Identifier: MIT

#pragma once

#include "errno.h"
#include "filesystem/vfs_types.h"


// Make a device special file; only works on certain filesystem types.
errno_t fs_mkdevfile(file_t at, char const *path, size_t path_len, devfile_t devfile);
