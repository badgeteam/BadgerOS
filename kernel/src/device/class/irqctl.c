
// SPDX-FileCopyrightText: 2025 Julian Scheffers <julian@scheffers.net>
// SPDX-FileType: SOURCE
// SPDX-License-Identifier: MIT

#include "device/class/irqctl.h"

#include "assertions.h"
#include "device/dev_class.h"



// Get the number of incoming interrupts.
size_t device_irqctl_incoming_len(device_t *device) {
    assert_always(device->dev_class == DEV_CLASS_IRQCTL);
    if (!device->driver)
        return 0;
    return ((device_irqctl_t *)device)->incoming_len;
}

// Enable an incoming interrupt.
bool device_irqctl_enable_in(device_t *device, size_t irq_in_pin, bool enabled) {
    assert_always(device->dev_class == DEV_CLASS_IRQCTL);
    if (!device->driver)
        return false;
    return ((driver_irqctl_t const *)device->driver)->enable_in(device, irq_in_pin, enabled);
}
