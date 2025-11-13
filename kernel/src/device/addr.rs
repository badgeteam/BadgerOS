// SPDX-FileCopyrightText: 2025 Julian Scheffers <julian@scheffers.net>
// SPDX-FileType: SOURCE
// SPDX-License-Identifier: MIT

/// Device address: Memory-mapped I/O.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub struct MmioAddr {
    /// Physical base address.
    pub paddr: usize,
    /// Virtual base address, if mapped.
    pub vaddr: usize,
    /// Region size in bytes.
    pub size: usize,
}

/// Device address: PCI or PCI express.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub struct PciAddr {
    /// PCI bus number (0-255).
    pub bus: u8,
    /// PCI device number on the bus (0-31).
    pub device: u8,
    /// PCI function number (0-7).
    pub function: u8,
}

/// Device address: AHCI drives.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub struct AhciAddr {
    /// AHCI controller port index (0-31).
    pub port: u8,
    /// AHCI port multiplier port index (optional 0-16).
    pub pmul_port: Option<u8>,
}

/// Represents addressing information required to access one of the functions of a device.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub enum DeviceAddr {
    Mmio(MmioAddr),
    Pci(PciAddr),
    I2c(u16),
    Ahci(AhciAddr),
}
