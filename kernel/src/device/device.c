
// SPDX-FileCopyrightText: 2025 Julian Scheffers <julian@scheffers.net>
// SPDX-FileType: SOURCE
// SPDX-License-Identifier: MIT

#include "device/device.h"

#include "arrays.h"
#include "assertions.h"
#include "badge_strings.h"
#include "cache.h"
#include "cpu/interrupt.h"
#include "device/class/union.h"
#include "device/dev_addr.h"
#include "device/dev_class.h"
#include "device/dtb/dtb.h"
#include "list.h"
#include "memprotect.h"
#include "mutex.h"
#include "port/hardware_allocation.h"
#include "set.h"
#include "spinlock.h"
#include "time.h"

#include <stdbool.h>
#include <stddef.h>



// ID -> device map.
static size_t           devs_len, devs_cap;
// ID -> device map.
static device_union_t **devs;
// Device ID counter.
static uint32_t         id_ctr;
// Devices list mutex.
static mutex_t          devs_mtx     = MUTEX_T_INIT_SHARED;
// Drivers set mutex.
static mutex_t          drivers_mtx  = MUTEX_T_INIT_SHARED;
// Set of drivers.
static set_t            drivers      = PTR_SET_EMPTY;
// Spinlock guarding interrupt graph changes.
static spinlock_t       irqconn_lock = SPINLOCK_T_INIT_SHARED;



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
    mutex_init(&device->base.driver_mtx, true);

    for (size_t i = 0; i < device->base.info.addrs_len; i++) {
        if (device->base.info.addrs[i].type == DEV_ATYPE_MMIO) {
            dev_mmio_addr_t *addr  = &device->base.info.addrs[i].mmio;
            size_t           vaddr = memprotect_alloc_vaddr(addr->size);
            if (!vaddr) {
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
// Returns `true` if the search for drivers should stop, regardless of whether adding this one was successful.
static bool device_add_to_driver(device_union_t *device, driver_t const *driver) {
    mutex_acquire(&device->base.driver_mtx, TIMESTAMP_US_MAX);

    if (device->base.state != DEV_STATE_ACTIVE) {
        // Cannot add a driver because the device is inactive.
        mutex_release(&device->base.driver_mtx);
        return true;
    }

    if (device->base.driver) {
        // Device had already received a driver.
        mutex_release(&device->base.driver_mtx);
        return true;
    }

    dev_class_t dev_class   = device->base.dev_class;
    bool        match_class = (dev_class == DEV_CLASS_UNKNOWN || dev_class == driver->dev_class);
    if (match_class && driver->match(&device->base.info) && driver->add(&device->base)) {
        device->base.dev_class = driver->dev_class;

        // Take the irq spinlock around the driver set to guard against partial write of the field during an interrupt.
        assert_dev_keep(irq_disable());
        spinlock_take(&irqconn_lock);
        device->base.driver = driver;
        spinlock_release(&irqconn_lock);
        irq_enable();

        mutex_release(&device->base.driver_mtx);
        return true;
    }

    mutex_release(&device->base.driver_mtx);
    return false;
}

// Remove a device from its driver.
// Frees all memory used by devices with drivers.
static void device_remove_from_driver(device_union_t *device) {
    mutex_acquire(&device->base.driver_mtx, TIMESTAMP_US_MAX);

    if (device->base.driver) {
        driver_t const *driver = device->base.driver;

        // Take the irq spinlock around the driver set to guard against partial write of the field during an interrupt.
        assert_dev_keep(irq_disable());
        spinlock_take(&irqconn_lock);
        device->base.driver = NULL;
        spinlock_release(&irqconn_lock);
        irq_enable();

        // Only actually remove from driver afterward to prevent an interrupt from getting to this driver mid-removal.
        driver->remove(&device->base);
    }

    mutex_release(&device->base.driver_mtx);
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
bool device_link_irq_impl(device_union_t *child, irqpin_t child_pin, device_union_t *parent, irqpin_t parent_pin) {
    // Enfore both devices are in the tree.
    if (!child->base.info.id || !parent->base.info.id) {
        return false;
    }

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
    dlist_append(&parent->base.irq_children[parent_pin], &conn->parent.node);

    return true;
}

// Remove a device interrupt link; see `device_link_irq`.
bool device_unlink_irq_impl(device_union_t *child, irqpin_t child_pin, device_union_t *parent, irqpin_t parent_pin) {
    // Enfore both devices are in the tree.
    if (!child->base.info.id || !parent->base.info.id) {
        return false;
    }

    // Find the interrupt connection.
    dlist_foreach(irqconn_t, conn, child.node, &child->base.irq_parents[child_pin]) {
        if (conn->parent.device == &parent->base && conn->parent.pin == parent_pin) {
            // Disable interrupt, if possible, on both sides.
            if (child->base.driver) {
                child->base.driver->enable_irq_out(&child->base, child_pin, false);
            }
            if (parent->base.driver) {
                parent->base.driver->enable_irq_in(&parent->base, parent_pin, false);
            }

            // Remove the connection from both.
            dlist_remove(&child->base.irq_parents[child_pin], &conn->child.node);
            dlist_remove(&parent->base.irq_children[parent_pin], &conn->parent.node);

            // Free memory.
            free(conn);

            return true;
        }
    }

    return false;
}


// Register a new device.
// Takes ownership of any memory in `info`, regardless of success.
// Returns a nonzero ID if successful.
device_t *device_add(device_info_t info) {
    device_union_t *device = calloc(1, sizeof(device_union_t));
    if (!device) {
        free(info.addrs);
        return NULL;
    }

    mutex_acquire(&devs_mtx, TIMESTAMP_US_MAX);

    // Insert device into the list.
    if (!array_lencap_insert(&devs, sizeof(void *), &devs_len, &devs_cap, &device, devs_len)) {
        free(info.addrs);
        free(device);
        mutex_release(&devs_mtx);
        return NULL;
    }

    // Initialize device data.
    uint32_t id           = ++id_ctr;
    device->base.info     = info;
    device->base.info.id  = id;
    device->base.refcount = 2;
    device->base.state    = DEV_STATE_INACTIVE;
    if (!device_init(device)) {
        free(info.addrs);
        free(device);
        mutex_release(&devs_mtx);
        return NULL;
    }

    // Add to parent's set of children.
    if (device->base.info.parent) {
        set_add(device->base.info.parent->children, device);
    }

    mutex_release(&devs_mtx);

    return &device->base;
}

// Activate a device; search for a driver for the device.
// If no driver could be found, the device is now eligible to get one in the future.
// If this function is not called, no driver will ever be added.
void device_activate(device_t *device) {
    mutex_acquire(&device->driver_mtx, TIMESTAMP_US_MAX);
    if (device->state == DEV_STATE_INACTIVE) {
        device->state = DEV_STATE_ACTIVE;
    }
    mutex_release(&device->driver_mtx);
    device_try_find_driver((device_union_t *)device);
}

// Remove a device and its children.
static uint32_t device_remove_impl(uint32_t id) {
    array_binsearch_t res = array_binsearch(devs, sizeof(void *), devs_len, (void *)(size_t)id, dev_id_search);

    if (res.found) {
        device_union_t *device = devs[res.index];

        // Disconnect from interrupt parents, if any.
        for (size_t i = 0; i < device->base.irq_parents_count; i++) {
            while (device->base.irq_parents[i].len) {
                irqconn_t conn = *(irqconn_t *)device->base.irq_parents[i].head;
                device_unlink_irq_impl(device, i, (device_union_t *)conn.parent.device, conn.parent.pin);
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



// List all devices; returns a `set_t` of `device_t *` shares.
// This reference must be cleaned up by `device_pop_ref` and can be cloned by `device_push_ref`.
set_t device_get_all() {
    set_t set = PTR_SET_EMPTY;
    mutex_acquire_shared(&devs_mtx, TIMESTAMP_US_MAX);

    for (size_t i = 0; i < devs_len; i++) {
        if (set_add(&set, devs[i])) {
            device_push_ref(&devs[i]->base);
        }
    }

    mutex_release_shared(&devs_mtx);
    return set;
}

static dev_addr_t mask_addr(dev_addr_t addr, dev_addr_t mask) {
    switch (addr.type) {
        case DEV_ATYPE_MMIO:
            addr.mmio.paddr &= mask.mmio.paddr;
            addr.mmio.vaddr &= mask.mmio.vaddr;
            addr.mmio.size  &= mask.mmio.size;
            break;
        case DEV_ATYPE_PCI:
            addr.pci.bus  &= mask.pci.bus;
            addr.pci.dev  &= mask.pci.dev;
            addr.pci.func &= mask.pci.func;
            break;
        case DEV_ATYPE_I2C: addr.i2c &= mask.i2c; break;
        case DEV_ATYPE_AHCI:
            addr.ahci.pmul_port &= mask.ahci.pmul_port;
            addr.ahci.port      &= mask.ahci.port;
            addr.ahci.pmul      &= mask.ahci.pmul;
            break;
    }
    return addr;
}

static bool match_addr(dev_addr_t a, dev_addr_t b) {
    switch (a.type) {
        case DEV_ATYPE_MMIO:
            return a.mmio.paddr == b.mmio.paddr && a.mmio.vaddr == b.mmio.vaddr && a.mmio.size == b.mmio.size;
        case DEV_ATYPE_PCI: return a.pci.bus == b.pci.bus && a.pci.dev == b.pci.dev && a.pci.func == b.pci.func;
        case DEV_ATYPE_I2C: return a.i2c == b.i2c;
        case DEV_ATYPE_AHCI:
            return a.ahci.pmul_port == b.ahci.pmul_port && a.ahci.port == b.ahci.port && a.ahci.pmul == b.ahci.pmul;
    }
    return false;
}

static bool match_filter(device_union_t const *device, dev_filter_t const *filter) {
    if (filter->match_class && device->base.dev_class != filter->class) {
        return false;
    }

    if (filter->match_addr) {
        bool has_addr = false;
        for (size_t i = 0; i < device->base.info.addrs_len; i++) {
            dev_addr_t addr = device->base.info.addrs[i];
            if (addr.type != filter->addr.type) {
                continue;
            }
            if (filter->use_addr_mask) {
                addr = mask_addr(addr, filter->addr_mask);
            }
            if (match_addr(addr, filter->addr)) {
                has_addr = true;
                break;
            }
        }
        if (!has_addr) {
            return false;
        }
    }

    if (filter->match_driver && device->base.driver != filter->driver) {
        return false;
    }

    if (filter->match_parent && !filter->parent_id && device->base.info.parent) {
        return false;
    }

    return true;
}

// List all devices by class that match the filter; returns a `set_t` of `device_t *` shares.
// This reference must be cleaned up by `device_pop_ref` and can be cloned by `device_push_ref`.
set_t device_get_filtered(dev_filter_t const *filter) {
    set_t set = PTR_SET_EMPTY;
    mutex_acquire_shared(&devs_mtx, TIMESTAMP_US_MAX);

    if (filter->match_parent && filter->parent_id) {
        device_t *parent = device_get(filter->parent_id);
        if (parent) {
            set_foreach(device_union_t, child, parent->children) {
                if (match_filter(child, filter)) {
                    if (set_add(&set, child)) {
                        device_push_ref(&child->base);
                    }
                }
            }
            device_pop_ref(parent);
        }
    } else {
        for (size_t i = 0; i < devs_len; i++) {
            if (match_filter(devs[i], filter)) {
                if (set_add(&set, devs[i])) {
                    device_push_ref(&devs[i]->base);
                }
            }
        }
    }

    mutex_release_shared(&devs_mtx);
    return set;
}



// Add a device interrupt link; child is the device that generates the interrupt, parent the one that receives it.
// Any device interrupt pin can be connected to any number of opposite pins, but the resulting graph must be acyclic.
// If a device has incoming interrupts then it must be an interrupt controller and only such drivers can match.
bool device_link_irq(device_t *child, irqpin_t child_pin, device_t *parent, irqpin_t parent_pin) {
    assert_dev_keep(irq_disable());
    spinlock_take(&irqconn_lock);
    bool success = device_link_irq_impl((device_union_t *)child, child_pin, (device_union_t *)parent, parent_pin);
    spinlock_release(&irqconn_lock);
    irq_enable();
    return success;
}

// Remove a device interrupt link; see `device_link_irq`.
bool device_unlink_irq(device_t *child, irqpin_t child_pin, device_t *parent, irqpin_t parent_pin) {
    assert_dev_keep(irq_disable());
    spinlock_take(&irqconn_lock);
    bool success = device_unlink_irq_impl((device_union_t *)child, child_pin, (device_union_t *)parent, parent_pin);
    spinlock_release(&irqconn_lock);
    irq_enable();
    return success;
}

// Enable an outgoing interrupt.
bool device_enable_irq_out(device_t *device, irqpin_t irq_out_pin, bool enabled) {
    mutex_acquire_shared(&device->driver_mtx, TIMESTAMP_US_MAX);
    bool success = device->driver && device->driver->enable_irq_out(device, irq_out_pin, enabled);
    mutex_release_shared(&device->driver_mtx);
    return success;
}

// Cascade-enable an interrupt output.
bool device_cascade_enable_irq_out(device_t *device, irqpin_t irq_out_pin) {
    mutex_acquire_shared(&device->driver_mtx, TIMESTAMP_US_MAX);
    if (!device->driver || !device->irq_parents[irq_out_pin].len ||
        !device->driver->enable_irq_out(device, irq_out_pin, true)) {
        mutex_release_shared(&device->driver_mtx);
        return false;
    }
    dlist_foreach(irqconn_t, conn, child.node, &device->irq_parents[irq_out_pin]) {
        device_t *parent = conn->parent.device;
        if (!parent->driver || !parent->driver->cascase_enable_irq) {
            continue;
        }
        parent->driver->cascase_enable_irq(parent, conn->parent.pin);
    }
    mutex_release_shared(&device->driver_mtx);
    return true;
}

// Enable an incoming interrupt.
bool device_enable_irq_in(device_t *device, irqpin_t irq_in_pin, bool enabled) {
    mutex_acquire_shared(&device->driver_mtx, TIMESTAMP_US_MAX);
    bool success =
        device->driver && device->driver->enable_irq_in && device->driver->enable_irq_in(device, irq_in_pin, enabled);
    mutex_release_shared(&device->driver_mtx);
    return success;
}

// Send an interrupt to all children on a certain pin.
void device_forward_interrupt(device_t *device, irqpin_t irq_in_pin) {
    assert_dev_drop(!irq_is_enabled());
    dlist_foreach(irqconn_t, conn, parent.node, &device->irq_children[irq_in_pin]) {
        if (conn->child.device->driver) {
            conn->child.device->driver->interrupt(conn->child.device, conn->child.pin);
        }
    }
}



// Notify of a device interrupt.
void device_interrupt(device_t *device, irqpin_t irq_pin) {
    assert_dev_drop(!irq_is_enabled());
    spinlock_take_shared(&irqconn_lock);
    if (device->driver) {
        device->driver->interrupt(device, irq_pin);
    }
    spinlock_release_shared(&irqconn_lock);
}



// Register a new driver.
bool driver_add(driver_t const *driver) {
    mutex_acquire(&drivers_mtx, TIMESTAMP_US_MAX);
    if (!set_add(&drivers, driver)) {
        mutex_release(&drivers_mtx);
        return false;
    }
    mutex_release(&drivers_mtx);

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
    mutex_acquire(&drivers_mtx, TIMESTAMP_US_MAX);
    if (!set_remove(&drivers, driver)) {
        mutex_release(&drivers_mtx);
        return false;
    }
    mutex_release(&drivers_mtx);

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
