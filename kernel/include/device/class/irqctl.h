
// SPDX-FileCopyrightText: 2025 Julian Scheffers <julian@scheffers.net>
// SPDX-FileType: SOURCE
// SPDX-License-Identifier: MIT

#pragma once

#include "device/device.h"



// Interrupt controller driver functions.
typedef struct {
    // Get the number of incoming interrupts.
    size_t (*incoming_len)(device_t *device);
    // Get the number of outgoing interrupts.
    size_t (*outgoing_len)(device_t *device);
    // Enable an incoming interrupt.
    void (*enable_in)(device_t *device, size_t irq_in_pin, bool enabled);
    // Enable an outgoing interrupt.
    void (*enable_out)(device_t *device, size_t irq_out_pin, bool enabled);
} driver_irqctl_t;
