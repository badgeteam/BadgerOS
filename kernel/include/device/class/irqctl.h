
// SPDX-FileCopyrightText: 2025 Julian Scheffers <julian@scheffers.net>
// SPDX-FileType: SOURCE
// SPDX-License-Identifier: MIT

#pragma once

#include "device/device.h"



// Interrupt controller device.
typedef struct {
    device_t          base;
    // Number of incoming interrupts.
    size_t            incoming_len;
    // Interrupt children.
    device_irqconn_t *irq_children;
} device_irqctl_t;

// Interrupt controller driver functions.
typedef struct {
    driver_t base;
    // Enable an incoming interrupt.
    bool (*enable_in)(device_t *device, size_t irq_in_pin, bool enabled);
} driver_irqctl_t;

// Get the number of incoming interrupts.
size_t device_irqctl_incoming_len(device_t *device);
// Enable an incoming interrupt.
bool   device_irqctl_enable_in(device_t *device, size_t irq_in_pin, bool enabled);
