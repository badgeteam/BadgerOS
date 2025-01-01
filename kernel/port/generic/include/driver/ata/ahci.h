
// SPDX-License-Identifier: MIT

#pragma once

#include "driver/ata.h"
#include "driver/ata/ahci/ahci_port.h"
#include "driver/ata/ahci/generic_host_ctrl.h"
#include "driver/ata/ahci/pci_cap.h"

// All HBA BAR registers - not meant to be constructed.
typedef struct {
    // Generic host control.
    ahci_bar_ghc_t  generic_host_ctrl;
    // Reserved.
    uint8_t         _reserved0[52];
    // Reserved for NVMHCI.
    uint8_t         _reserved1[64];
    // Vendor-specific registers.
    uint8_t         _reserved2[96];
    // Port control registers.
    ahci_bar_port_t ports[32];
} ahci_bar_t;
_Static_assert(offsetof(ahci_bar_t, ports) == 0x100, "Offset of ports in ahci_bar_t must be 0x100");

// SATA controller handle.
typedef struct {
    // Base class.
    ata_handle_t     base;
    // PCI address.
    pci_addr_t       addr;
    // PCI BAR handle.
    pci_bar_handle_t bar;
    // Enabled ports.
    uint32_t         ports_enabled;
    // Pointer to HBA BAR registers.
    ahci_bar_t      *regs;
} sata_handle_t;