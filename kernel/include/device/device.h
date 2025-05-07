
// SPDX-FileCopyrightText: 2025 Julian Scheffers <julian@scheffers.net>
// SPDX-FileType: SOURCE
// SPDX-License-Identifier: MIT

#pragma once

#include "device/dev_addr.h"
#include "device/dev_class.h"
#include "set.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>



// A single connected device.
typedef struct device         device_t;
// A device interrupt pin connection.
typedef struct device_irqconn irqconn_t;
// A device driver.
typedef struct driver         driver_t;

// A single connected device.
struct device {
    // Parent device, if any.
    device_t       *parent;
    // Globally unique device ID.
    uint32_t        id;
    // Device address.
    dev_addr_t      addr;
    // What class of device this is.
    dev_class_t     dev_class;
    // Assigned driver.
    driver_t const *driver;
    // Set of children.
    set_t          *children;
    // Number of outgoing interrupts.
    size_t          irq_count;
    // Outgoing interrupt connections.
    irqconn_t      *irq_parents;
};

// A device interrupt pin connection.
struct device_irqconn {
    // Connected device; must be a `device_irqctl_t` for interrupt parents.
    device_t *device;
    // Connected device's incoming pin.
    size_t    pin;
};

// A device driver.
struct driver {
    // What class of devices this driver targets.
    dev_class_t dev_class;
    // Try to match this driver against a certain device.
    bool (*match)(device_t *device);
    // Register a new device to this driver.
    void (*add)(device_t *device);
    // Remove a device from this driver.
    void (*remove)(device_t *device);
    // Device interrupt handler.
    void (*interrupt)(device_t *device, size_t irq_pin);
    // Enable a certain interrupt.
    void (*enable_irq)(device_t *device, size_t irq_pin, bool enable);
};



// Register a new device.
// The `driver` field may be NULL and the `children` field must be NULL.
// Updates the device's ID if successful.
bool device_add(device_t *device);
// Remove a device and its children.
bool device_remove(uint32_t id);
// Notify of a device interrupt.
void device_interrupt(device_t *device, size_t irq_pin);

// Register a new driver.
bool driver_add(driver_t const *driver);
// Remove a driver.
bool driver_remove(driver_t const *driver);
