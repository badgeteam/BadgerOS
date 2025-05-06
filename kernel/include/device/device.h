
// SPDX-FileCopyrightText: 2025 Julian Scheffers <julian@scheffers.net>
// SPDX-FileType: SOURCE
// SPDX-License-Identifier: MIT

#pragma once

#include "device/address.h"
#include "device/dev_class.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>



// A single connected device.
typedef struct device device_t;
// A device driver.
typedef struct driver driver_t;

// A single connected device.
struct device {
    // Device address.
    dev_addr_t      addr;
    // What class of device this is.
    dev_class_t     dev_class;
    // TODO: Assigned driver.
    driver_t const *driver;
};

// A device driver.
struct driver {
    // What class of devices this driver targets.
    dev_class_t dev_class;
};
