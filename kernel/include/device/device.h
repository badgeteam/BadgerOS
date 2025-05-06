
// SPDX-FileCopyrightText: 2025 Julian Scheffers <julian@scheffers.net>
// SPDX-FileType: SOURCE
// SPDX-License-Identifier: MIT

#pragma once

#include "device/address.h"



// Represents a single connected device.
typedef struct device device_t;

// Represents a single connected device.
struct device {
    // Device address.
    dev_addr_t addr;
    // TODO: Assigned driver.
    void      *driver;
};
