
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    // Page table root physical page number.
    size_t root_ppn;
} mpu_ctx_t;
