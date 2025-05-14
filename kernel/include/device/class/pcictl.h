
// SPDX-FileCopyrightText: 2025 Julian Scheffers <julian@scheffers.net>
// SPDX-FileType: SOURCE
// SPDX-License-Identifier: MIT

#pragma once

#include "device/device.h"



// PCI controller device; must use CAM (PCI) or ECAM (PCIe).
typedef struct {
    device_t base;
    // Is a PCIe controller (as opposed to a PCI controller).
    bool     is_pcie;
} device_pcictl_t;

// PCI controller device driver functions.
typedef struct {
    driver_t base;
    // Read data from the configuration space.
    void (*cam_read)(device_pcictl_t *device, uint32_t addr, uint32_t len, void *data);
    // Write data to the configuration space.
    void (*cam_write)(device_pcictl_t *device, uint32_t addr, uint32_t len, void *const data);
} driver_pcictl_t;



// Enumerate a PCI or PCIe bus, adding or removing devices accordingly.
void device_pcictl_enumerate(device_pcictl_t *device);
