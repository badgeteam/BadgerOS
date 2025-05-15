
// SPDX-FileCopyrightText: 2025 Julian Scheffers <julian@scheffers.net>
// SPDX-FileType: SOURCE
// SPDX-License-Identifier: MIT

#include "device/class/pcictl.h"

#include "arrays.h"
#include "device/dev_addr.h"
#include "device/device.h"
#include "device/pci/confspace.h"
#include "log.h"



static void device_pcictl_dev_detect(device_pcictl_t *device, uint8_t bus, uint8_t dev) {
    // Read first function info.
    driver_pcictl_t const *driver = (void *)device->base.driver;
    pcie_hdr_com_t         hdr;
    driver->cam_read(device, (bus * 256 + dev * 8) * 4096, sizeof(pcie_hdr_com_t), &hdr);

    if (hdr.vendor_id == 0xffff) {
        // No device at this location.
        return;
    }

    // Construct device info to contain all function numbers.
    device_info_t info = {
        .addrs      = NULL,
        .addrs_len  = 0,
        .dtb_handle = NULL,
        .dtb_node   = NULL,
        .parent     = &device->base,
    };

    // Insert function into device addresses.
    dev_addr_t addr = {
        .type     = DEV_ATYPE_PCI,
        .pci.bus  = bus,
        .pci.dev  = dev,
        .pci.func = 0,
    };
    if (!array_len_insert(&info.addrs, sizeof(dev_addr_t), &info.addrs_len, &addr, info.addrs_len)) {
        return;
    }

    bool multifunc = hdr.hdr_type & 0x80;
    if (multifunc) {
        for (uint8_t func = 1; func < 8; func++) {
            driver->cam_read(device, (bus * 256 + dev * 8 + func) * 4096, sizeof(pcie_hdr_com_t), &hdr);
            if (hdr.vendor_id == 0xffff) {
                // No secondary function at this location.
                continue;
            }

            // Insert function into device addresses.
            addr.pci.func = func;
            if (!array_len_insert(&info.addrs, sizeof(dev_addr_t), &info.addrs_len, &addr, info.addrs_len)) {
                free(info.addrs);
                return;
            }
        }
    }

    // Register new device.
    logkf(LOG_INFO, "Detected PCI device with %{size;d} function%{cs}", info.addrs_len, info.addrs_len == 1 ? "" : "s");
    for (size_t i = 0; i < info.addrs_len; i++) {
        logkf(
            LOG_INFO,
            "  -> %{u8;x}:%{u8;x}.%{u8;d}",
            info.addrs[i].pci.bus,
            info.addrs[i].pci.dev,
            info.addrs[i].pci.func
        );
    }
    device_add(info);
}

// Enumerate a PCI or PCIe bus, adding or removing devices accordingly.
void device_pcictl_enumerate(device_pcictl_t *device) {
    mutex_acquire_shared(&device->base.driver_mtx, TIMESTAMP_US_MAX);
    if (!device->base.driver) {
        mutex_release_shared(&device->base.driver_mtx);
        return;
    }
    for (unsigned bus = device->bus_start; bus <= device->bus_end; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            device_pcictl_dev_detect(device, bus, dev);
        }
    }
    mutex_release_shared(&device->base.driver_mtx);
}
