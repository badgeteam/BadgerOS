
// SPDX-FileCopyrightText: 2025 Julian Scheffers <julian@scheffers.net>
// SPDX-FileType: SOURCE
// SPDX-License-Identifier: MIT

#include "device/class/irqctl.h"

#include "device/device.h"
#include "list.h"



// Enable an incoming interrupt.
bool device_irqctl_enable_in(device_irqctl_t *device, irqpin_t irq_in_pin, bool enabled) {
    if (!device->base.driver)
        return false;
    return ((driver_irqctl_t const *)device->base.driver)->enable_in(device, irq_in_pin, enabled);
}

// Send an interrupt to all children on a certain pin.
void device_irqctl_forward_interrupt(device_irqctl_t *device, irqpin_t irq_in_pin) {
    dlist_foreach(irqconn_t, conn, parent.node, &device->irq_children[irq_in_pin]) {
        device_interrupt(conn->child.device, conn->child.pin);
    }
}
