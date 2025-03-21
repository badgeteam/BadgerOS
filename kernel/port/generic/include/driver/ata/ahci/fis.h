
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>



// FIS types.
typedef enum {
    // Register FIS - host to device.
    FIS_TYPE_REG_H2D   = 0x27,
    // Register FIS - device to host.
    FIS_TYPE_REG_D2H   = 0x34,
    // DMA activate FIS - device to host.
    FIS_TYPE_DMA_ACT   = 0x39,
    // DMA setup FIS - bidirectional.
    FIS_TYPE_DMA_SETUP = 0x41,
    // Data FIS - bidirectional.
    FIS_TYPE_DATA      = 0x46,
    // BIST activate FIS - bidirectional.
    FIS_TYPE_BIST      = 0x58,
    // PIO setup FIS - device to host.
    FIS_TYPE_PIO_SETUP = 0x5F,
    // Set device bits FIS - device to host.
    FIS_TYPE_DEV_BITS  = 0xA1,
} fis_type_t;

// Host to device FIS data.
typedef struct {
    // DWORD 0
    uint8_t fis_type; // FIS_TYPE_REG_H2D

    uint8_t pmport : 4; // Port multiplier
    uint8_t        : 3; // Reserved
    uint8_t is_cmd : 1; // 1: Command, 0: Control

    uint8_t command;  // Command register
    uint8_t featurel; // Feature register, 7:0

    // DWORD 1
    uint8_t lba0;   // LBA low register, 7:0
    uint8_t lba1;   // LBA mid register, 15:8
    uint8_t lba2;   // LBA high register, 23:16
    uint8_t device; // Device register

    // DWORD 2
    uint8_t lba3;     // LBA register, 31:24
    uint8_t lba4;     // LBA register, 39:32
    uint8_t lba5;     // LBA register, 47:40
    uint8_t featureh; // Feature register, 15:8

    // DWORD 3
    uint8_t countl;  // Count register, 7:0
    uint8_t counth;  // Count register, 15:8
    uint8_t icc;     // Isochronous command completion
    uint8_t control; // Control register

    // DWORD 4
    uint32_t : 32; // Reserved
} fis_h2d_t;

// Device to host FIS data.
typedef struct {
    // DWORD 0
    uint8_t fis_type; // FIS_TYPE_REG_D2H

    uint8_t pmport : 4; // Port multiplier
    uint8_t        : 2; // Reserved
    uint8_t irq    : 1; // Interrupt bit
    uint8_t        : 1; // Reserved

    uint8_t status; // Status register
    uint8_t error;  // Error register

    // DWORD 1
    uint8_t lba0;   // LBA low register, 7:0
    uint8_t lba1;   // LBA mid register, 15:8
    uint8_t lba2;   // LBA high register, 23:16
    uint8_t device; // Device register

    // DWORD 2
    uint8_t lba3; // LBA register, 31:24
    uint8_t lba4; // LBA register, 39:32
    uint8_t lba5; // LBA register, 47:40
    uint8_t : 8;  // Reserved

    // DWORD 3
    uint8_t countl; // Count register, 7:0
    uint8_t counth; // Count register, 15:8
    uint16_t : 16;  // Reserved

    // DWORD 4
    uint32_t : 32; // Reserved
} fis_d2h_t;

// Bidirectional data FIS.
typedef struct {
    // DWORD 0
    uint8_t fis_type; // FIS_TYPE_DATA

    uint8_t pmport : 4; // Port multiplier
    uint8_t        : 4; // Reserved
    uint8_t        : 8; // Reserved

    // DWORD 1 ~ N
    uint32_t data[]; // Payload
} fis_data_t;

// PIO setup FIS data.
typedef struct {
    // DWORD 0
    uint8_t fis_type; // FIS_TYPE_PIO_SETUP

    uint8_t pmport : 4; // Port multiplier
    uint8_t        : 1; // Reserved
    uint8_t dir    : 1; // Data transfer direction, 1 - device to host
    uint8_t irq    : 1; // Interrupt bit
    uint8_t rsv1   : 1;

    uint8_t status; // Status register
    uint8_t error;  // Error register

    // DWORD 1
    uint8_t lba0;   // LBA low register, 7:0
    uint8_t lba1;   // LBA mid register, 15:8
    uint8_t lba2;   // LBA high register, 23:16
    uint8_t device; // Device register

    // DWORD 2
    uint8_t lba3; // LBA register, 31:24
    uint8_t lba4; // LBA register, 39:32
    uint8_t lba5; // LBA register, 47:40
    uint8_t : 8;  // Reserved

    // DWORD 3
    uint8_t countl;   // Count register, 7:0
    uint8_t counth;   // Count register, 15:8
    uint8_t : 8;      // Reserved
    uint8_t e_status; // New value of status register

    // DWORD 4
    uint16_t tcount; // Transfer count
    uint16_t : 16;   // Reserved
} fis_pio_setup_t;

// DMA setup FIS data.
typedef struct {
    // DWORD 0
    uint8_t fis_type; // FIS_TYPE_DMA_SETUP

    uint8_t pmport        : 4; // Port multiplier
    uint8_t               : 1; // Reserved
    uint8_t dir           : 1; // Data transfer direction, 1 - device to host
    uint8_t irq           : 1; // Interrupt bit
    uint8_t auto_activate : 1; // Auto-activate. Specifies if DMA Activate FIS is needed

    uint8_t rsved[2]; // Reserved

    // DWORD 1&2
    uint64_t dma_buf_id; // DMA Buffer Identifier. Used to Identify DMA buffer in host memory.
                         // SATA Spec says host specific and not in Spec. Trying AHCI spec might work.

    // DWORD 3
    uint32_t : 32; // More reserved

    // DWORD 4
    uint32_t dma_buf_offset; // Byte offset into buffer. First 2 bits must be 0

    // DWORD 5
    uint32_t tcount; // Number of bytes to transfer. Bit 0 must be 0

    // DWORD 6
    uint32_t : 32; // Reserved
} fis_dma_setup_t;
