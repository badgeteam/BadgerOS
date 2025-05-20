
// SPDX-License-Identifier: MIT

#pragma once

#include "filesystem/vfs_types.h"



// Make a device special file; only works on certain filesystem types.
void fs_mkdevfile(badge_err_t *ec, file_t at, char const *path, size_t path_len, devfile_t devfile);
