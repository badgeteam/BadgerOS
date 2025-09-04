
// SPDX-FileCopyrightText: 2025 Julian Scheffers <julian@scheffers.net>
// SPDX-FileType: SOURCE
// SPDX-License-Identifier: MIT

#pragma once

#include "device/dev_addr.h"
#include "device/dev_class.h"
#include "device/dtb/dtb.h"
#include "errno.h"
#include "filesystem.h"
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

// Device interrupt designator.
typedef uint32_t            irqno_t;
// All information required to match drivers with devices and install said drivers.
typedef struct device_info  device_info_t;
// A single connected device.
typedef struct device       device_t;
// Array of `devirqno_t`.
typedef struct devirqno_arr devirqno_arr_t;
// Pair of owned `device_t *` and `irqno_t`.
typedef struct devirqno     devirqno_t;
// A device interrupt connection.
typedef struct irqconn      irqconn_t;
// One or more device interrupt connections.
typedef struct irqconns     irqconns_t;
// A device driver.
typedef struct driver       driver_t;
// Device filter.
typedef struct dev_filter   dev_filter_t;

// All information required to match drivers with devices and install said drivers.
struct device_info {
    // Parent device, if any.
    device_t     *parent;
    // Interrupt parent device, if any.
    // Should not be set for devices that forward interrupts to multiple different interrupt controllers.
    device_t     *irq_parent;
    // Number of device addresses, usually at least 1.
    size_t        addrs_len;
    // Device addresses.
    dev_addr_t   *addrs;
    // DTB handle, if any.
    // Not owned by this info struct as the DTB is never freed.
    dtb_handle_t *dtb_handle;
    // DTB node, if any.
    // Not owned by this info struct as the DTB is never freed.
    dtb_node_t   *dtb_node;
    // Copy of phandle for DTB nodes, or 0 if none.
    uint32_t      phandle;
};

// A single connected device.
struct device {
    // Device info.
    device_info_t   info;
    // Globally unique device ID; becomes invalid when the device is removed.
    uint32_t        id;
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
    size_t          irq_parents_len;
    // Capacity for outgoing interrupts.
    size_t          irq_parents_cap;
    // Interrupt parents; lists of `irqconn_t`.
    // Can be read from interrupts; guarded by `irqconn_lock`.
    irqconns_t     *irq_parents;
    // Number of incoming interrupts.
    size_t          irq_children_len;
    // Capacity for incoming interrupts.
    size_t          irq_children_cap;
    // Interrupt children; lists of `irqconn_t`.
    // Can be read from interrupts; guarded by `irqconn_lock`.
    irqconns_t     *irq_children;
    // Additional driver-specific data, if any.
    void           *cookie;
};

// Array of `devirqno_t`.
struct devirqno_arr {
    size_t      len;
    devirqno_t *arr;
};

// Pair of owned `device_t *` and `irqno_t`.
struct devirqno {
    // Connected device.
    device_t *device;
    // Connected device's interrupt.
    irqno_t   irqno;
};

// A device interrupt connection.
struct irqconn {
    // Linked list node in parent device.
    dlist_node_t node;
    // Connected device.
    device_t    *device;
    // Connected device's interrupt.
    irqno_t      irqno;
};

// One or more device interrupt connections.
struct irqconns {
    // This device's incoming/outgoing interrupt desginator.
    irqno_t irqno;
    // Doubly-linked list of connections to other devices' interrupt designators; list of `irqconn_t`.
    dlist_t connections;
};

// A device driver.
struct driver {
    // What class of devices this driver targets.
    dev_class_t dev_class;
    // Try to match this driver against a certain device.
    bool (*match)(device_info_t *info);
    // Register a new device to this driver.
    errno_t (*add)(device_t *device);
    // Remove a device from this driver.
    void (*remove)(device_t *device);
    // [optional] Called after a direct child device is added with `device_add`.
    // If this fails, the child is removed again.
    errno_t (*child_added)(device_t *device, device_t *child_device);
    // [optional] Called after a direct child is activated with `device_activate`.
    void (*child_activated)(device_t *device, device_t *child_device);
    // [optional] Called after a direct child device gets added to a driver.
    void (*child_got_driver)(device_t *device, device_t *child_device);
    // [optional] Called before a direct child device gets removed from a driver.
    // Always called before `child_removed`.
    void (*child_lost_driver)(device_t *device, device_t *child_device);
    // [optional] Called before a direct child device is removed with `device_remove`.
    void (*child_removed)(device_t *device, device_t *child_device);
    // Device interrupt handler; also responsible for any potential forwarding of interrupts.
    // Only called from an interrupt context.
    // Returns true if this handled an interrupt request.
    bool (*interrupt)(device_t *device, irqno_t irqno);
    // Enable a certain interrupt output.
    // Can be called with interrupts disabled.
    errno_t (*enable_irq_out)(device_t *device, irqno_t irqno, bool enable);
    // [optional] Enable an incoming interrupt.
    // Can be called with interrupts disabled.
    errno_t (*enable_irq_in)(device_t *device, irqno_t in_irqno, bool enable);
    // [optional] Cascade-enable interrupts from some input designator.
    // Can be called with interrupts disabled.
    errno_t (*cascase_enable_irq)(device_t *device, irqno_t in_irqno);
    // [optional] Create additional device node files.
    // Called when a new `devtmpfs` is mounted OR after registered to the driver.
    errno_t (*create_devnodes)(device_t *device, file_t devtmpfs_root, file_t devnode_dir);
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

// Drivers set mutex.
extern mutex_t drivers_mtx;
// Set of drivers.
extern set_t   drivers;



// Test a device info against a set of DTB compatible strings.
bool      device_test_dtb_compat(device_info_t const *info, size_t compats_len, char const *const *compats);
// Register a new device.
// Takes ownership of any memory in `info`, regardless of success.
// Returns a nonzero ID if successful.
device_t *device_add(device_info_t info);
// Set the driver for a device explicitly.
// Returns EEXIST if it already has one.
errno_t   device_set_driver(device_t *device, driver_t const *driver);
// Activate a device; search for a driver for the device.
// If no driver could be found, the device is now eligible to get one in the future.
// If this function is not called, no driver will ever be added.
void      device_activate(device_t *device);
// Remove a device and its children.
bool      device_remove(uint32_t id);
// Try to get a reference to a device by ID.
// This reference must be cleaned up by `device_pop_ref` and can be cloned by `device_push_ref`.
device_t *device_by_id(uint32_t id);
// Try to get a reference to a device by DTB phandle.
// This reference must be cleaned up by `device_pop_ref` and can be cloned by `device_push_ref`.
device_t *device_by_phandle(uint32_t phandle);
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
// Any device interrupt designator can be connected to any number of opposite designators, but the resulting graph must
// be acyclic.
errno_t        device_link_irq(device_t *child, irqno_t child_irqno, device_t *parent, irqno_t parent_irqno);
// Remove a device interrupt link; see `device_link_irq`.
errno_t        device_unlink_irq(device_t *child, irqno_t child_irqno, device_t *parent, irqno_t parent_irqno);
// Get the list of incoming IRQ links.
devirqno_arr_t device_list_in_irq(device_t *device, irqno_t in_irqno);
// Get the list of outgoing IRQ links.
devirqno_arr_t device_list_out_irq(device_t *device, irqno_t out_irqno);
// Get a set containing all connected incoming interrupts.
set_t          device_all_in_irq(device_t *device);
// Get a set containing all connected outgoing interrupts.
set_t          device_all_out_irq(device_t *device);
// Free all memory and device references of an `devirqno_arr_t`.
void           devirqno_arr_free(devirqno_arr_t arr);

// Enable an outgoing interrupt.
errno_t device_enable_irq_out(device_t *device, irqno_t out_irqno, bool enabled);
// Cascade-enable an interrupt output's connected parent pins.
// Does not actually enable this interrupt pin on `device`.
errno_t device_cascade_enable_irq_out(device_t *device, irqno_t out_irqno);
// Enable an incoming interrupt.
errno_t device_enable_irq_in(device_t *device, irqno_t in_irqno, bool enabled);
// Helper to send an interrupt to all children on a certain designator.
// Returns true if an interrupt handler was run.
bool    device_forward_interrupt(device_t *device, irqno_t in_irqno);

// Notify of a device interrupt.
void device_interrupt(device_t *device, irqno_t irq_irqno);

// Register a new driver.
errno_t driver_add(driver_t const *driver);
// Remove a driver.
errno_t driver_remove(driver_t const *driver);

// [implemented in Rust] Create the DevNull and DevZero and add them to the tree.
void device_create_null_zero();
