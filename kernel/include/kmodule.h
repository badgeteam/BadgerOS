
// SPDX-FileCopyrightText: 2025 Julian Scheffers <julian@scheffers.net>
// SPDX-FileType: SOURCE
// SPDX-License-Identifier: MIT

#pragma once



// Kernel module interface.
typedef struct {
    // Module name, [a-zA-Z0-9_].
    char const *name;
    // Module init function.
    void (*init)();
    // Module deinit function.
    void (*deinit)();
} kmodule_t;
