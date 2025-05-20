
// SPDX-FileCopyrightText: 2025 Julian Scheffers <julian@scheffers.net>
// SPDX-FileType: SOURCE
// SPDX-License-Identifier: MIT

#pragma once

#include "device/dev_addr.h"
#include "device/dev_class.h"
#include "device/dtb/dtb.h"
#include "mutex.h"
#include "set.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>



// Device state.
typedef enum {
    // Inactive; being set up.
    DEV_STATE_INACTIVE,
    // Active; eligible to receive a driver.
    DEV_STATE_ACTIVE,
    // Removed from the device tree.
    DEV_STATE_REMOVED,
} dev_state_t;

// Device interrupt pin number.
typedef uint16_t           irqpin_t;
// All information required to match drivers with devices and install said drivers.
typedef struct device_info device_info_t;
// A single connected device.
typedef struct device      device_t;
// A device interrupt pin connection.
typedef struct irqconn     irqconn_t;
// One or more device interrupt pin connections.
typedef struct irqconns    irqconns_t;
// A device driver.
typedef struct driver      driver_t;
// Device filter.
typedef struct dev_filter  dev_filter_t;

// All information required to match drivers with devices and install said drivers.
struct device_info {
    // Parent device, if any.
    device_t     *parent;
    // Globally unique device ID; if 0, the device is not in the tree and cannot be used.
    uint32_t      id;
    // Number of device addresses, usually at least 1.
    size_t        addrs_len;
    // Device addresses.
    dev_addr_t   *addrs;
    // DTB handle, if any.
    dtb_handle_t *dtb_handle;
    // DTB node, if any.
    dtb_node_t   *dtb_node;
};

// A single connected device.
struct device {
    // Device info.
    device_info_t   info;
    // Current device state.
    dev_state_t     state;
    // Device reference count; when it reaches 0, the struct is freed.
    atomic_int      refcount;
    // What class of device this is; must be equal to that of the driver.
    dev_class_t     dev_class;
    // Mutex guarding the driver.
    mutex_t         driver_mtx;
    // Assigned driver.
    driver_t const *driver;
    // Set of children.
    set_t          *children;
    // Number of outgoing interrupts.
    size_t          irq_parents_count;
    // Interrupt parents; lists of `irqconn_t`.
    dlist_t        *irq_parents;
    // Number of incoming interrupts.
    size_t          irq_children_len;
    // Interrupt children; lists of `irqconn_t`.
    dlist_t        *irq_children;
    // Additional driver-specific data, if any.
    void           *cookie;
};

// A device interrupt pin connection.
struct irqconn {
    struct {
        // Linked list node in parent device.
        dlist_node_t node;
        // Connected device.
        device_t    *device;
        // Connected device's interrupt pin.
        irqpin_t     pin;
    } parent, child;
};

// A device driver.
struct driver {
    // What class of devices this driver targets.
    dev_class_t dev_class;
    // Try to match this driver against a certain device.
    bool (*match)(device_info_t *info);
    // Register a new device to this driver.
    bool (*add)(device_t *device);
    // Remove a device from this driver.
    void (*remove)(device_t *device);
    // Device interrupt handler; also responsible for any potential forwarding of interrupts.
    // Only called from an interrupt context.
    void (*interrupt)(device_t *device, irqpin_t irq_pin);
    // Enable a certain interrupt output.
    // Can be called with interrupts disabled.
    bool (*enable_irq_out)(device_t *device, irqpin_t irq_pin, bool enable);
    // [optional] Enable an incoming interrupt.
    // Can be called with interrupts disabled.
    bool (*enable_irq_in)(device_t *device, irqpin_t irq_in_pin, bool enabled);
    // [optional] Cascade-enable interrupts from some input pin.
    // Can be called with interrupts disabled.
    void (*cascase_enable_irq)(device_t *device, irqpin_t irq_in_pin);
};

// Device filter.
struct dev_filter {
    // Whether to match class.
    bool match_class;
    // Whether to match addr.
    bool match_addr;
    // Whether to match driver.
    bool match_driver;
    // Whether to match parent.
    bool match_parent;
    // Whether to use address mask.
    bool use_addr_mask;
    // Class to match.
    dev_class_t class;
    // Addr to match.
    dev_addr_t      addr;
    // Mask for addr to match.
    dev_addr_t      addr_mask;
    // Driver to match.
    driver_t const *driver;
    // ID of parent device to match.
    uint32_t        parent_id;
};



// Test a device info against a set of DTB compatible strings.
bool      device_test_dtb_compat(device_info_t const *info, size_t compats_len, char const *const *compats);
// Register a new device.
// Takes ownership of any memory in `info`, regardless of success.
// Returns a nonzero ID if successful.
device_t *device_add(device_info_t info);
// Activate a device; search for a driver for the device.
// If no driver could be found, the device is now eligible to get one in the future.
// If this function is not called, no driver will ever be added.
void      device_activate(device_t *device);
// Remove a device and its children.
bool      device_remove(uint32_t id);
// Try to get a reference to a device by ID.
// This reference must be cleaned up by `device_pop_ref` and can be cloned by `device_push_ref`.
device_t *device_get(uint32_t id);
// Decrease device reference count.
void      device_pop_ref(device_t *device);
// Increase device reference count.
void      device_push_ref(device_t *device);

// List all devices; returns a `set_t` of `device_t *` shares.
// This reference must be cleaned up by `device_pop_ref` and can be cloned by `device_push_ref`.
set_t device_get_all();
// List all devices by class that match the filter; returns a `set_t` of `device_t *` shares.
// This reference must be cleaned up by `device_pop_ref` and can be cloned by `device_push_ref`.
set_t device_get_filtered(dev_filter_t const *filter);

// Add a device interrupt link; child is the device that generates the interrupt, parent the one that receives it.
// Any device interrupt pin can be connected to any number of opposite pins, but the resulting graph must be acyclic.
// If a device has incoming interrupts then it must be an interrupt controller and only such drivers can match.
bool device_link_irq(device_t *child, irqpin_t child_pin, device_t *parent, irqpin_t parent_pin);
// Remove a device interrupt link; see `device_link_irq`.
bool device_unlink_irq(device_t *child, irqpin_t child_pin, device_t *parent, irqpin_t parent_pin);
// Enable an outgoing interrupt.
bool device_enable_irq_out(device_t *device, irqpin_t irq_out_pin, bool enabled);
// Cascade-enable an interrupt output.
bool device_cascade_enable_irq_out(device_t *device, irqpin_t irq_out_pin);
// Enable an incoming interrupt.
bool device_enable_irq_in(device_t *device, irqpin_t irq_in_pin, bool enabled);
// Helper to send an interrupt to all children on a certain pin.
void device_forward_interrupt(device_t *device, irqpin_t irq_in_pin);

// Notify of a device interrupt.
void device_interrupt(device_t *device, irqpin_t irq_pin);

// Register a new driver.
bool driver_add(driver_t const *driver);
// Remove a driver.
bool driver_remove(driver_t const *driver);
