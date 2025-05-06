
// SPDX-FileCopyrightText: 2025 Julian Scheffers <julian@scheffers.net>
// SPDX-FileType: SOURCE
// SPDX-License-Identifier: MIT

#pragma once

#include <stdint.h>



// Kernel module semver major.
#define KMODULE_ABI_MAJ 1
// Kernel module semver minor.
#define KMODULE_ABI_MIN 0
// Kernel module semver patch.
#define KMODULE_ABI_PAT 0



// Kernel module interface.
typedef struct {
    // Minimum kernel module semver (maj, min, pat).
    uint8_t     min_abi[3];
    // Module name, [a-zA-Z0-9_].
    char const *name;
    // Module init function.
    void (*init)();
    // Module deinit function.
    void (*deinit)();
} kmodule_t;
