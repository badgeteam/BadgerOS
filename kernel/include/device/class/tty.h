
// SPDX-FileCopyrightText: 2025 Julian Scheffers <julian@scheffers.net>
// SPDX-FileType: SOURCE
// SPDX-License-Identifier: MIT

#pragma once

#include "device/device.h"



// TTY device.
typedef struct {
    device_t base;
} device_tty_t;

// TTY device driver functions.
typedef struct {
    driver_t base;
} driver_tty_t;
