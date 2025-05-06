
// SPDX-FileCopyrightText: 2025 Julian Scheffers <julian@scheffers.net>
// SPDX-FileType: SOURCE
// SPDX-License-Identifier: MIT

#pragma once



// Distinguishes classes of device by purpose.
typedef enum {
    // Block devices, e.g. SSDs, CD drives, etc.
    DEV_CLASS_BLOCK,
    // Interrupt controllers.
    DEV_CLASS_IRQCTL,
    // TTY devices, e.g. USB CDC-ACM, kernel console, etc.
    DEV_CLASS_TTY,
} dev_class_t;
