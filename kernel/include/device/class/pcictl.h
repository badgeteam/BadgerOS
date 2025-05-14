
// SPDX-FileCopyrightText: 2025 Julian Scheffers <julian@scheffers.net>
// SPDX-FileType: SOURCE
// SPDX-License-Identifier: MIT

#pragma once

#include "device/device.h"
#include "device/pci/bar.h"



// Physical address -> BAR mapping range.
typedef struct {
    // CPU physical address.
    size_t      cpu_paddr;
    // PCI BAR space address.
    pci_paddr_t pci_paddr;
    // Region length in bytes.
    size_t      length;
} pci_bar_range_t;

// Mapping from PCI IRQ to CPU IRQ.
typedef struct {
    // PCI device address.
    pci_paddr_t pci_paddr;
    // PCI IRQ pin.
    int         pci_irq;
    // CPU IRQ pin.
    int         cpu_irq;
} pci_irqmap_t;

// PCI controller device; must use CAM (PCI) or ECAM (PCIe).
typedef struct {
    device_t         base;
    // Is a PCIe controller (as opposed to a PCI controller).
    bool             is_pcie;
    // Number of BAR ranges.
    size_t           ranges_len;
    // Physical address -> BAR mapping ranges.
    pci_bar_range_t *ranges;
    // Number of PCI IRQ to CPU IRQ mappings.
    size_t           irqmap_len;
    // PCI IRQ to CPU IRQ mappings.
    pci_irqmap_t    *irqmap;
    // Interrupt mask.
    pci_paddr_t      irqmap_mask;
    // First bus number.
    uint8_t          bus_start;
    // Last bus number.
    uint8_t          bus_end;
} device_pcictl_t;

// PCI controller device driver functions.
typedef struct {
    driver_t base;
    // Read data from the configuration space.
    void (*cam_read)(device_pcictl_t *device, uint32_t addr, uint32_t len, void *data);
    // Write data to the configuration space.
    void (*cam_write)(device_pcictl_t *device, uint32_t addr, uint32_t len, void const *data);
} driver_pcictl_t;



// Enumerate a PCI or PCIe bus, adding or removing devices accordingly.
void device_pcictl_enumerate(device_pcictl_t *device);
