
// SPDX-FileCopyrightText: 2025 Julian Scheffers <julian@scheffers.net>
// SPDX-FileType: SOURCE
// SPDX-License-Identifier: MIT

#include "device/device.h"

#include "arrays.h"
#include "assertions.h"
#include "badge_strings.h"
#include "cache.h"
#include "cpu/interrupt.h"
#include "device/class/block.h"
#include "device/class/irqctl.h"
#include "device/class/union.h"
#include "device/dev_addr.h"
#include "device/dev_class.h"
#include "device/dtb/dtb.h"
#include "list.h"
#include "memprotect.h"
#include "mutex.h"
#include "port/hardware_allocation.h"
#include "set.h"
#include "time.h"

#include <stddef.h>



// ID -> device map.
static size_t           devs_len, devs_cap;
// ID -> device map.
static device_union_t **devs;
// Device ID counter.
static uint32_t         id_ctr;
// Devices list mutex.
static mutex_t          devs_mtx = MUTEX_T_INIT_SHARED;
// Set of drivers.
static set_t            drivers  = PTR_SET_EMPTY;



// Binary search for device by ID comparator.
static int dev_id_search(void const *a, void const *b) {
    device_union_t const *dev = *(void **)a;
    uint32_t              id  = (size_t)b;
    if (dev->base.info.id < id) {
        return -1;
    } else if (dev->base.info.id > id) {
        return 1;
    } else {
        return 0;
    }
}

// Find a device without taking the mutex.
static device_union_t *device_get_unsafe(uint32_t id) {
    array_binsearch_t res = array_binsearch(devs, sizeof(void *), devs_len, (void *)(size_t)id, dev_id_search);
    return res.found ? devs[res.index] : NULL;
}



// Test a device info against a set of DTB compatible strings.
bool device_test_dtb_compat(device_info_t const *info, size_t compats_len, char const *const *compats) {
    if (!info->dtb_node) {
        return false;
    }
    dtb_prop_t *prop = dtb_get_prop(info->dtb_handle, info->dtb_node, "compatible");
    if (!prop) {
        return false;
    }
    uint32_t offset = 0;
    while (offset < prop->content_len) {
        uint32_t len = cstr_length_upto(prop->content + offset, prop->content_len - offset);
        for (size_t i = 0; i < compats_len; i++) {
            if (cstr_length(compats[i]) == len && mem_equals(prop->content + offset, compats[i], len)) {
                return true;
            }
        }
        offset += len + 1;
    }
    return false;
}

// Initialize generic information used by devices, with or without drivers.
static bool device_init(device_union_t *device) {
    device->base.children  = malloc(sizeof(set_t));
    *device->base.children = PTR_SET_EMPTY;

    for (size_t i = 0; i < device->base.info.addrs_len; i++) {
        if (device->base.info.addrs[i].type == DEV_ATYPE_MMIO) {
            dev_mmio_addr_t *addr  = &device->base.info.addrs[i].mmio;
            size_t           vaddr = memprotect_alloc_vaddr(addr->size);
            if (!addr->vaddr) {
                for (i--; i != SIZE_MAX; i--) {
                    if (device->base.info.addrs[i].type == DEV_ATYPE_MMIO) {
                        memprotect_free_vaddr(addr->vaddr);
                        addr->vaddr = 0;
                    }
                }
                return false;
            }
            addr->vaddr = vaddr + addr->paddr % MEMMAP_PAGE_SIZE;
        }
    }
    for (size_t i = 0; i < device->base.info.addrs_len; i++) {
        if (device->base.info.addrs[i].type == DEV_ATYPE_MMIO) {
            dev_mmio_addr_t addr  = device->base.info.addrs[i].mmio;
            addr.size            += addr.vaddr % MEMMAP_PAGE_SIZE;
            addr.paddr           -= addr.vaddr % MEMMAP_PAGE_SIZE;
            addr.vaddr           -= addr.vaddr % MEMMAP_PAGE_SIZE;
            if (addr.size % MEMMAP_PAGE_SIZE) {
                addr.size += MEMMAP_PAGE_SIZE - addr.size % MEMMAP_PAGE_SIZE;
            }
            assert_dev_keep(memprotect_k(addr.vaddr, addr.paddr, addr.size, MEMPROTECT_FLAG_IO | MEMPROTECT_FLAG_RW));
        }
    }
    memprotect_commit(&mpu_global_ctx);

    return true;
}

// Free all generic information used by devices, with or without drivers.
static void device_deinit(device_union_t *device) {
    assert_dev_drop(device->base.children->len == 0);

    for (size_t i = 0; i < device->base.info.addrs_len; i++) {
        if (device->base.info.addrs[i].type == DEV_ATYPE_MMIO) {
            dev_mmio_addr_t addr  = device->base.info.addrs[i].mmio;
            addr.size            += addr.vaddr % MEMMAP_PAGE_SIZE;
            addr.paddr           -= addr.vaddr % MEMMAP_PAGE_SIZE;
            addr.vaddr           -= addr.vaddr % MEMMAP_PAGE_SIZE;
            if (addr.size % MEMMAP_PAGE_SIZE) {
                addr.size += MEMMAP_PAGE_SIZE - addr.size % MEMMAP_PAGE_SIZE;
            }
            assert_dev_keep(memprotect_k(addr.vaddr, addr.paddr, addr.size, 0));
        }
    }
    memprotect_commit(&mpu_global_ctx);
    for (size_t i = 0; i < device->base.info.addrs_len; i++) {
        if (device->base.info.addrs[i].type == DEV_ATYPE_MMIO) {
            memprotect_free_vaddr(device->base.info.addrs[i].mmio.vaddr);
        }
    }

    free(device->base.children);
    free(device->base.info.addrs);
}

// Register a device to a certain driver.
// Initializes all data used by devices with drivers.
static bool device_add_to_driver(device_union_t *device, driver_t const *driver) {
    dev_class_t dev_class   = device->base.dev_class;
    bool        match_class = (dev_class == DEV_CLASS_UNKNOWN || dev_class == driver->dev_class);
    if (match_class && driver->match(&device->base.info) && driver->add(&device->base)) {
        device->base.dev_class = driver->dev_class;
        device->base.driver    = driver;
        return true;
    }
    return false;
}

// Remove a device from a certain driver.
// Frees all memory used by devices with drivers.
static void device_remove_from_driver(device_union_t *device) {
    assert_dev_drop(device->base.driver);
    device->base.driver->remove(&device->base);
    device->base.driver = NULL;
}

// Search for a driver for a device and if found, add it to that driver.
static void device_try_find_driver(device_union_t *device) {
    set_foreach(driver_t const, driver, &drivers) {
        if (device_add_to_driver(device, driver)) {
            return;
        }
    }
}

// Add a device interrupt link; child is the device that generates the interrupt, parent the one that receives it.
// Any device interrupt pin can be connected to any number of opposite pins, but the resulting graph must be acyclic.
// If a device has incoming interrupts then it must be an interrupt controller and only such drivers can match.
bool device_link_irq_impl(device_union_t *child, irqpin_t child_pin, device_irqctl_t *parent, irqpin_t parent_pin) {
    // Enfore both devices are in the tree.
    if (!child->base.info.id || !parent->base.info.id) {
        return false;
    }

    // Enforce that the parent is classified as an interrupt controller.
    if (parent->base.dev_class != DEV_CLASS_IRQCTL && parent->base.dev_class != DEV_CLASS_UNKNOWN) {
        return false;
    }
    parent->base.dev_class = DEV_CLASS_IRQCTL;

    // Create a struct to represent the connection.
    irqconn_t *conn = calloc(1, sizeof(irqconn_t));
    if (!conn) {
        return false;
    }
    conn->child.device  = &child->base;
    conn->child.pin     = child_pin;
    conn->parent.device = &parent->base;
    conn->parent.pin    = parent_pin;

    // Register the connection to both.
    dlist_append(&child->base.irq_parents[child_pin], &conn->child.node);
    dlist_append(&parent->irq_children[parent_pin], &conn->parent.node);

    return true;
}

// Remove a device interrupt link; see `device_link_irq`.
bool device_unlink_irq_impl(device_union_t *child, irqpin_t child_pin, device_irqctl_t *parent, irqpin_t parent_pin) {
    // Enfore both devices are in the tree.
    if (!child->base.info.id || !parent->base.info.id) {
        return false;
    }

    // Enforce that the parent is classified as an interrupt controller.
    if (parent->base.dev_class != DEV_CLASS_IRQCTL) {
        return false;
    }

    // Find the interrupt connection.
    dlist_foreach(irqconn_t, conn, child.node, &child->base.irq_parents[child_pin]) {
        if (conn->parent.device == &parent->base && conn->parent.pin == parent_pin) {
            // Disable interrupt, if possible, on both sides.
            if (child->base.driver) {
                child->base.driver->enable_irq(&child->base, child_pin, false);
            }
            if (parent->base.driver) {
                ((driver_irqctl_t const *)parent->base.driver)->enable_in(parent, parent_pin, false);
            }

            // Remove the connection from both.
            dlist_remove(&child->base.irq_parents[child_pin], &conn->child.node);
            dlist_remove(&parent->irq_children[parent_pin], &conn->parent.node);

            // Free memory.
            free(conn);

            return true;
        }
    }

    return false;
}


// Register a new device.
// Returns a nonzero ID if successful.
uint32_t device_add(device_info_t info) {
    device_union_t *device = calloc(1, sizeof(device_union_t));
    if (!device) {
        free(info.addrs);
        return 0;
    }

    mutex_acquire(&devs_mtx, TIMESTAMP_US_MAX);

    // Insert device into the list.
    if (!array_lencap_insert(&devs, sizeof(void *), &devs_len, &devs_cap, &device, devs_len)) {
        free(info.addrs);
        free(device);
        mutex_release(&devs_mtx);
        return 0;
    }

    // Initialize device data.
    uint32_t id           = ++id_ctr;
    device->base.info     = info;
    device->base.info.id  = id;
    device->base.refcount = 1;
    if (!device_init(device)) {
        free(info.addrs);
        free(device);
        mutex_release(&devs_mtx);
        return 0;
    }

    // Add to parent's set of children.
    if (device->base.info.parent) {
        set_add(device->base.info.parent->children, device);
    }

    // Try to find a matching driver.
    device_try_find_driver(device);

    mutex_release(&devs_mtx);

    return id;
}

// Remove a device and its children.
static uint32_t device_remove_impl(uint32_t id) {
    array_binsearch_t res = array_binsearch(devs, sizeof(void *), devs_len, (void *)(size_t)id, dev_id_search);

    if (res.found) {
        device_union_t *device = devs[res.index];

        // Disconnect from interrupt parents, if any.
        for (size_t i = 0; i < device->base.irq_count; i++) {
            while (device->base.irq_parents[i].len) {
                irqconn_t conn = *(irqconn_t *)device->base.irq_parents[i].head;
                device_unlink_irq_impl(device, i, (device_irqctl_t *)conn.parent.device, conn.parent.pin);
            }
        }

        // First remove child devices, if any.
        if (device->base.children) {
            set_foreach(device_union_t, child, device->base.children) {
                device_remove_impl(child->base.info.id);
            }
        }

        if (device->base.driver) {
            // Children removed, remove the device itself.
            device_remove_from_driver(device);
        }

        // Remove from parent's set of children.
        if (device->base.info.parent) {
            set_remove(device->base.info.parent->children, device);
        }

        // Remove the device from the list.
        array_lencap_remove(&devs, sizeof(void *), &devs_len, &devs_cap, NULL, res.index);
        device_pop_ref((device_t *)device);
    }

    return res.found;
}

// Remove a device and its children.
bool device_remove(uint32_t id) {
    mutex_acquire(&devs_mtx, TIMESTAMP_US_MAX);
    bool success = device_remove_impl(id);
    mutex_release(&devs_mtx);
    return success;
}

// Try to get a reference to a device by ID.
// This reference must be cleaned up by `device_pop_ref` and can be cloned by `device_push_ref`.
device_t *device_get(uint32_t id) {
    mutex_acquire_shared(&devs_mtx, TIMESTAMP_US_MAX);

    device_t *dev = (device_t *)device_get_unsafe(id);
    if (dev) {
        dev->refcount++;
    }

    mutex_release_shared(&devs_mtx);

    return dev;
}

// Decrease device reference count.
void device_pop_ref(device_t *device_base) {
    device_union_t *device = (device_union_t *)device_base;
    if (--device->base.refcount != 0) {
        return;
    }

    switch (device->base.dev_class) {
        case DEV_CLASS_UNKNOWN: /* NOLINT; no action required. */ break;
        case DEV_CLASS_BLOCK: cache_clear(&device->block.cache); break;
        case DEV_CLASS_IRQCTL: /* NOLINT; no action required. */ break;
        case DEV_CLASS_TTY: /* NOLINT; no action required. */ break;
        case DEV_CLASS_PCICTL: /* NOLINT; no action required. */ break;
        case DEV_CLASS_I2CCTL: /* NOLINT; no action required. */ break;
        case DEV_CLASS_AHCI: /* NOLINT; no action required. */ break;
    }
    device_deinit(device);
    free(device);
}

// Increase device reference count.
void device_push_ref(device_t *device) {
    device->refcount++;
}



// Add a device interrupt link; child is the device that generates the interrupt, parent the one that receives it.
// Any device interrupt pin can be connected to any number of opposite pins, but the resulting graph must be acyclic.
// If a device has incoming interrupts then it must be an interrupt controller and only such drivers can match.
bool device_link_irq(device_t *child, irqpin_t child_pin, device_t *parent, irqpin_t parent_pin) {
    return device_link_irq_impl((device_union_t *)child, child_pin, (device_irqctl_t *)parent, parent_pin);
}

// Remove a device interrupt link; see `device_link_irq`.
bool device_unlink_irq(device_t *child, irqpin_t child_pin, device_t *parent, irqpin_t parent_pin) {
    return device_unlink_irq_impl((device_union_t *)child, child_pin, (device_irqctl_t *)parent, parent_pin);
}

// Notify of a device interrupt.
void device_interrupt(device_t *device, irqpin_t irq_pin) {
    assert_dev_drop(!irq_is_enabled());
    if (device->driver) {
        device->driver->interrupt(device, irq_pin);
    }
}



// Register a new driver.
bool driver_add(driver_t const *driver) {
    if (!set_add(&drivers, driver)) {
        return false;
    }

    // Try to match driverless devices against this driver.
    mutex_acquire(&devs_mtx, TIMESTAMP_US_MAX);
    for (size_t i = 0; i < devs_len; i++) {
        if (device_add_to_driver(devs[i], driver)) {
            break;
        }
    }
    mutex_release(&devs_mtx);

    return true;
}

// Remove a driver.
bool driver_remove(driver_t const *driver) {
    if (!set_remove(&drivers, driver)) {
        return false;
    }

    // Remove all devices from this driver.
    for (size_t i = 0; i < devs_len; i++) {
        if (devs[i]->base.driver == driver) {
            device_remove_from_driver(devs[i]);

            // Try to find an alternative driver if possible.
            device_try_find_driver(devs[i]);
        }
    }

    return true;
}
