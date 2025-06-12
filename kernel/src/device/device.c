
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
#include "device/class/union.h"
#include "device/dev_addr.h"
#include "device/dev_class.h"
#include "device/dtb/dtb.h"
#include "errno.h"
#include "filesystem.h"
#include "list.h"
#include "log.h"
#include "map.h"
#include "memprotect.h"
#include "mutex.h"
#include "nanoprintf.h"
#include "set.h"
#include "spinlock.h"
#include "time.h"
#include "todo.h"

#include <stdbool.h>
#include <stddef.h>



// ID -> device map.
static map_t    devs_by_id      = PTR_MAP_EMPTY;
// Phandle -> device map.
static map_t    devs_by_phandle = PTR_MAP_EMPTY;
// Device ID counter.
static uint32_t id_ctr;
// Devices list mutex.
mutex_t         devs_mtx     = MUTEX_T_INIT_SHARED;
// Drivers set mutex.
static mutex_t  drivers_mtx  = MUTEX_T_INIT_SHARED;
// Set of drivers.
static set_t    drivers      = PTR_SET_EMPTY;
// Mutex guarding the act of changing the interrupt graph.
mutex_t         irqconn_mtx  = MUTEX_T_INIT_SHARED;
// Spinlock guarding changes to interrupt graph fields.
spinlock_t      irqconn_lock = SPINLOCK_T_INIT_SHARED;



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
            addr->vaddr = vaddr + addr->paddr % CONFIG_PAGE_SIZE;
        }
    }
    for (size_t i = 0; i < device->base.info.addrs_len; i++) {
        if (device->base.info.addrs[i].type == DEV_ATYPE_MMIO) {
            dev_mmio_addr_t addr  = device->base.info.addrs[i].mmio;
            addr.size            += addr.vaddr % CONFIG_PAGE_SIZE;
            addr.paddr           -= addr.vaddr % CONFIG_PAGE_SIZE;
            addr.vaddr           -= addr.vaddr % CONFIG_PAGE_SIZE;
            if (addr.size % CONFIG_PAGE_SIZE) {
                addr.size += CONFIG_PAGE_SIZE - addr.size % CONFIG_PAGE_SIZE;
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
            addr.size            += addr.vaddr % CONFIG_PAGE_SIZE;
            addr.paddr           -= addr.vaddr % CONFIG_PAGE_SIZE;
            addr.vaddr           -= addr.vaddr % CONFIG_PAGE_SIZE;
            if (addr.size % CONFIG_PAGE_SIZE) {
                addr.size += CONFIG_PAGE_SIZE - addr.size % CONFIG_PAGE_SIZE;
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


// Called after a driver's add function succeeds.
static void driver_add_succeeded(device_union_t *device) {
    switch (device->base.dev_class) {
        case DEV_CLASS_UNKNOWN: break;
        case DEV_CLASS_BLOCK: {
            if (!device->block.no_cache) {
                device_block_init_cache(&device->block);
            }
        } break;
        case DEV_CLASS_IRQCTL:
        case DEV_CLASS_TTY:
        case DEV_CLASS_PCICTL:
        case DEV_CLASS_I2CCTL:
        case DEV_CLASS_AHCI: break;
    }
}


// Register a device to a certain driver.
// Initializes all data used by devices with drivers.
// Returns `true` if the search for drivers should stop, regardless of whether adding this one was successful.
static bool device_add_to_driver(device_t *device, driver_t const *driver) {
    mutex_acquire(&device->driver_mtx, TIMESTAMP_US_MAX);

    if (device->state != DEV_STATE_ACTIVE) {
        // Cannot add a driver because the device is inactive.
        mutex_release(&device->driver_mtx);
        return true;
    }

    if (device->driver) {
        // Device had already received a driver.
        mutex_release(&device->driver_mtx);
        return true;
    }

    dev_class_t dev_class   = device->dev_class;
    bool        match_class = (dev_class == DEV_CLASS_UNKNOWN || dev_class == driver->dev_class);
    if (match_class && driver->match(&device->info)) {
        if (dev_class == DEV_CLASS_UNKNOWN) {
            device->dev_class = driver->dev_class;
        }
        errno_t res = driver->add(device);
        if (res >= 0) {
            device->dev_class = driver->dev_class;

            // Take the irq spinlock around the driver set to guard against partial write of the field during an
            // interrupt.
            assert_dev_keep(irq_disable());
            spinlock_take(&irqconn_lock);
            device->driver = driver;
            spinlock_release(&irqconn_lock);
            irq_enable();

            driver_add_succeeded((device_union_t *)device);

            device_t *parent = device->info.parent;
            if (parent) {
                mutex_acquire_shared(&parent->driver_mtx, TIMESTAMP_US_MAX);
                mutex_release(&device->driver_mtx);
                if (parent->driver && parent->driver->child_got_driver) {
                    parent->driver->child_got_driver(parent, device);
                }
                mutex_release_shared(&parent->driver_mtx);
            } else {
                mutex_release(&device->driver_mtx);
            }

            return true;
        } else {
            logkf(LOG_ERROR, "driver->add failed: %{cs} (%{cs})", errno_get_name(-res), errno_get_desc(-res));
            if (dev_class == DEV_CLASS_UNKNOWN) {
                device->dev_class = DEV_CLASS_UNKNOWN;
            }
        }
    }

    mutex_release(&device->driver_mtx);
    return false;
}

// Remove a device from its driver.
// Frees all memory used by devices with drivers.
static void device_remove_from_driver(device_t *device) {
    mutex_acquire(&device->driver_mtx, TIMESTAMP_US_MAX);

    if (device->driver) {
        driver_t const *driver = device->driver;

        device_t *parent = device->info.parent;
        if (parent && parent->driver && parent->driver->child_lost_driver) {
            parent->driver->child_lost_driver(parent, device);
        }

        // Take the irq spinlock around the driver set to guard against partial write of the field during an interrupt.
        assert_dev_keep(irq_disable());
        spinlock_take(&irqconn_lock);
        device->driver = NULL;
        spinlock_release(&irqconn_lock);
        irq_enable();

        // Only actually remove from driver afterward to prevent an interrupt from getting to this driver mid-removal.
        driver->remove(device);
    }

    mutex_release(&device->driver_mtx);
}

// Search for a driver for a device and if found, add it to that driver.
static void device_try_find_driver(device_t *device) {
    set_foreach(driver_t const, driver, &drivers) {
        if (device_add_to_driver(device, driver)) {
            return;
        }
    }
}

// Remove a device interrupt link; see `device_link_irq`.
// Reuses `irqconn_t::child.node` to store nodes in `to_free_list`.
static errno_t device_unlink_irq_impl(device_t *child, irqno_t child_pin, device_t *parent, irqno_t parent_pin);

// Register a new device.
// Takes ownership of any memory in `info`, regardless of success.
// Returns a nonzero ID if successful.
device_t *device_add(device_info_t info) {
    device_union_t *device = calloc(1, sizeof(device_union_t));
    if (!device) {
        goto err0;
    }

    mutex_acquire(&devs_mtx, TIMESTAMP_US_MAX);

    // Insert device into the list.
    uint32_t id = ++id_ctr;
    if (!map_set(&devs_by_id, (void *)(size_t)id, device)) {
        goto err1;
    }
    if (info.phandle && !map_set(&devs_by_phandle, (void *)(size_t)info.phandle, device)) {
        goto err2;
    }

    // Initialize device data.
    device->base.info     = info;
    device->base.id       = id;
    device->base.refcount = 1;
    device->base.state    = DEV_STATE_INACTIVE;
    if (!device_init(device)) {
        goto err3;
    }

    // Add to parent's set of children.
    device_t *parent = device->base.info.parent;
    if (parent) {
        if (!set_add(parent->children, device)) {
            goto err4;
        }
        if (parent->driver && parent->driver->child_added) {
            errno_t res = parent->driver->child_added(parent, &device->base);
            if (res < 0) {
                logkf(
                    LOG_ERROR,
                    "parent->driver->child_added failed: %{cs} (%{cs})",
                    errno_get_name(-res),
                    errno_get_desc(-res)
                );
                goto err4;
            }
        }
    }

    device->base.refcount = 2;
    mutex_release(&devs_mtx);

    return &device->base;

err4:
    if (info.phandle) {
        map_remove(&devs_by_phandle, (void *)(size_t)info.phandle);
    }
    map_remove(&devs_by_id, (void *)(size_t)id);
    device_pop_ref(&device->base);
    mutex_release(&devs_mtx);
    return NULL;

err3:
    if (info.phandle) {
        map_remove(&devs_by_phandle, (void *)(size_t)info.phandle);
    }
err2:
    map_remove(&devs_by_id, (void *)(size_t)id);
err1:
    free(device);
err0:
    device_pop_ref(info.parent);
    device_pop_ref(info.irq_parent);
    free(info.addrs);
    mutex_release(&devs_mtx);
    return NULL;
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
    device_try_find_driver(device);
    device_t *parent = device->info.parent;
    if (parent) {
        if (parent->driver && parent->driver->child_activated) {
            parent->driver->child_activated(parent, device);
        }
    }
}

// Remove a device and its children.
// Reuses `irqconn_t::child.node` to store nodes in `to_free_list`.
static uint32_t device_remove_impl(uint32_t id) {
    device_t *device = map_get(&devs_by_id, (void *)(size_t)id);

    if (device) {
        // Disconnect from interrupt parents, if any.
        mutex_acquire(&irqconn_mtx, TIMESTAMP_US_MAX);
        for (size_t i = 0; i < device->irq_parents_len; i++) {
            while (device->irq_parents[i].connections.len) {
                irqconn_t conn = *(irqconn_t *)device->irq_parents[i].connections.head;
                device_unlink_irq_impl(device, i, conn.device, conn.irqno);
            }
        }
        mutex_release(&irqconn_mtx);

        // First remove child devices, if any.
        if (device->children) {
            set_foreach(device_t, child, device->children) {
                device_remove_impl(child->id);
            }
        }

        if (device->driver) {
            // Children removed, remove the device itself.
            device_remove_from_driver(device);
        }

        // Remove from parent's set of children.
        device_t *parent = device->info.parent;
        if (parent) {
            if (parent->driver && parent->driver->child_removed) {
                parent->driver->child_removed(parent, device);
            }
            set_remove(parent->children, device);
        }

        // Remove the device from the list.
        map_remove(&devs_by_id, (void *)(size_t)id);
        device_pop_ref((device_t *)device);
    }

    return device != NULL;
}

// Remove a device and its children.
bool device_remove(uint32_t id) {
    // Recursively remove devices.
    mutex_acquire(&devs_mtx, TIMESTAMP_US_MAX);
    bool success = device_remove_impl(id);
    mutex_release(&devs_mtx);

    return success;
}

// Try to get a reference to a device by ID.
// This reference must be cleaned up by `device_pop_ref` and can be cloned by `device_push_ref`.
device_t *device_by_id(uint32_t id) {
    mutex_acquire_shared(&devs_mtx, TIMESTAMP_US_MAX);

    device_t *dev = map_get(&devs_by_id, (void *)(size_t)id);
    if (dev) {
        dev->refcount++;
    }

    mutex_release_shared(&devs_mtx);
    return dev;
}

// Try to get a reference to a device by DTB phandle.
// This reference must be cleaned up by `device_pop_ref` and can be cloned by `device_push_ref`.
device_t *device_by_phandle(uint32_t phandle) {
    mutex_acquire_shared(&devs_mtx, TIMESTAMP_US_MAX);

    device_t *dev = map_get(&devs_by_phandle, (void *)(size_t)phandle);
    if (dev) {
        dev->refcount++;
    }

    mutex_release_shared(&devs_mtx);
    return dev;
}

// Decrease device reference count.
void device_pop_ref(device_t *device_base) {
    if (!device_base) {
        return;
    }

    device_union_t *device = (device_union_t *)device_base;
    if (--device->base.refcount != 0) {
        return;
    }

    if (device->base.info.parent) {
        device_pop_ref(device->base.info.parent);
    }
    if (device->base.info.irq_parent) {
        device_pop_ref(device->base.info.irq_parent);
    }

    switch (device->base.dev_class) {
        case DEV_CLASS_UNKNOWN: /* NOLINT; no action required. */ break;
        case DEV_CLASS_BLOCK: {
            if (!device->block.no_cache) {
                cache_clear(&device->block.cache);
            }
        } break;
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

    map_foreach(ent, &devs_by_id) {
        if (set_add(&set, ent->value)) {
            device_push_ref(ent->value);
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
        device_t *parent = device_by_id(filter->parent_id);
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
        map_foreach(ent, &devs_by_id) {
            if (match_filter(ent->value, filter)) {
                if (set_add(&set, ent->value)) {
                    device_push_ref(ent->value);
                }
            }
        }
    }

    mutex_release_shared(&devs_mtx);
    return set;
}



static int irqconns_irqno_cmp(void const *a0, void const *b0) {
    irqconns_t const *a = a0;
    irqconns_t const *b = b0;
    if (a->irqno < b->irqno) {
        return -1;
    } else if (a->irqno > b->irqno) {
        return 1;
    } else {
        return 0;
    }
}

// Helper for safe growing of the interrupt designators dictionary.
__attribute__((always_inline)) static inline errno_size_t
    device_alloc_irqno(size_t *const len, size_t *const cap, irqconns_t **const arr, irqno_t irqno) {
    irqconns_t        dummy = {.irqno = irqno, .connections = DLIST_EMPTY};
    array_binsearch_t res   = array_binsearch(*arr, sizeof(irqconns_t), *len, &dummy, irqconns_irqno_cmp);
    if (res.found) {
        return res.index;
    } else if (*cap >= *len + 1) {
        // No need to grow the array; just lock and insert.
        assert_dev_keep(irq_disable());
        spinlock_take(&irqconn_lock);

        array_insert(*arr, sizeof(irqconns_t), *len, &dummy, res.index);
        ++*len;

        spinlock_release(&irqconn_lock);
        irq_enable();

        return res.index;
    }

    size_t      new_cap = (*cap ?: 1) * 2;
    irqconns_t *mem     = ENOMEM_ON_NULL(calloc(new_cap, sizeof(irqconns_t)));

    assert_dev_keep(irq_disable());
    spinlock_take(&irqconn_lock);

    mem_copy(mem, *arr, res.index * sizeof(irqconns_t));
    mem[res.index] = dummy;
    mem_copy(mem + res.index + 1, *arr + res.index, (*len - res.index) * sizeof(irqconns_t));

    *arr = mem;
    *cap = new_cap;
    ++*len;

    spinlock_release(&irqconn_lock);
    irq_enable();

    return res.index;
}

// Add a device interrupt link; child is the device that generates the interrupt, parent the one that receives it.
// Any device interrupt pin can be connected to any number of opposite pins, but the resulting graph must be acyclic.
// If a device has incoming interrupts then it must be an interrupt controller and only such drivers can match.
errno_t device_link_irq(device_t *child, irqno_t child_pin, device_t *parent, irqno_t parent_pin) {
    // Enfore both devices are in the tree.
    if (child->state == DEV_STATE_REMOVED || parent->state == DEV_STATE_REMOVED) {
        return -EINVAL;
    }

    // Allocate a new interrupt connection.
    irqconn_t *child_conn  = ENOMEM_ON_NULL(malloc(sizeof(irqconn_t)));
    irqconn_t *parent_conn = ENOMEM_ON_NULL(malloc(sizeof(irqconn_t)), free(child_conn));

    mutex_acquire(&irqconn_mtx, TIMESTAMP_US_MAX);

    // Ensure enough capacity for interrupt connections.
    size_t child_index = RETURN_ON_ERRNO(
        device_alloc_irqno(&child->irq_parents_len, &child->irq_parents_cap, &child->irq_parents, child_pin),
        free(child_conn);
        free(parent_conn)
    );
    size_t parent_index = RETURN_ON_ERRNO(
        device_alloc_irqno(&parent->irq_children_len, &parent->irq_children_cap, &parent->irq_children, parent_pin),
        free(child_conn);
        free(parent_conn)
    );

    child_conn->device  = child;
    child_conn->irqno   = child_pin;
    parent_conn->device = parent;
    parent_conn->irqno  = parent_pin;

    assert_dev_keep(irq_disable());
    spinlock_take(&irqconn_lock);

    // Register the connection to both.
    dlist_append(&child->irq_parents[child_index].connections, &parent_conn->node);
    dlist_append(&parent->irq_children[parent_index].connections, &child_conn->node);

    spinlock_release(&irqconn_lock);
    irq_enable();

    mutex_release(&irqconn_mtx);
    return 0;
}

// Remove a device interrupt link; see `device_link_irq`.
static errno_t device_unlink_irq_impl(device_t *child, irqno_t child_irqno, device_t *parent, irqno_t parent_irqno) {
    TODO();
}

// Remove a device interrupt link; see `device_link_irq`.
errno_t device_unlink_irq(device_t *child, irqno_t child_irqno, device_t *parent, irqno_t parent_irqno) {
    // Enfore both devices are in the tree.
    if (child->state == DEV_STATE_REMOVED || parent->state == DEV_STATE_REMOVED) {
        return -EINVAL;
    }

    mutex_acquire(&irqconn_mtx, TIMESTAMP_US_MAX);

    errno_t res = device_unlink_irq_impl(child, child_irqno, parent, parent_irqno);

    mutex_release(&irqconn_mtx);

    return res;
}

static devirqno_arr_t device_list_irq_impl(device_t *device, irqno_t irqno, bool is_in) {
    devirqno_arr_t result = {0, NULL};
    mutex_acquire_shared(&irqconn_mtx, TIMESTAMP_US_MAX);
    irqconns_t        dummy     = {.irqno = irqno};
    irqconns_t       *conns     = is_in ? device->irq_children : device->irq_parents;
    size_t            conns_len = is_in ? device->irq_children_len : device->irq_parents_len;
    array_binsearch_t idx       = array_binsearch(conns, sizeof(irqconns_t), conns_len, &dummy, irqconns_irqno_cmp);
    if (!idx.found) {
        mutex_release_shared(&irqconn_mtx);
        return result;
    }
    size_t count = conns[idx.index].connections.len;
    if (!count) {
        mutex_release_shared(&irqconn_mtx);
        return result;
    }
    devirqno_t *arr = malloc(count * sizeof(devirqno_t));
    if (!arr) {
        mutex_release_shared(&irqconn_mtx);
        return result;
    }
    size_t i = 0;
    dlist_foreach_node(irqconn_t, conn, &conns[idx.index].connections) {
        device_push_ref(conn->device);
        arr[i].device = conn->device;
        arr[i].irqno  = conn->irqno;
        i++;
    }
    result.len = count;
    result.arr = arr;
    mutex_release_shared(&irqconn_mtx);
    return result;
}

// Get the list of incoming IRQ links.
devirqno_arr_t device_list_in_irq(device_t *device, irqno_t in_irqno) {
    return device_list_irq_impl(device, in_irqno, true);
}

// Get the list of outgoing IRQ links.
devirqno_arr_t device_list_out_irq(device_t *device, irqno_t out_irqno) {
    return device_list_irq_impl(device, out_irqno, false);
}

// Free all memory and device references of an `devirqno_arr_t`.
void devirqno_arr_free(devirqno_arr_t arr) {
    for (size_t i = 0; i < arr.len; i++) {
        device_pop_ref(arr.arr[i].device);
    }
    free(arr.arr);
}


// Get a set containing all connected incoming interrupts.
set_t device_all_in_irq(device_t *device) {
    set_t set = PTR_SET_EMPTY;
    mutex_acquire_shared(&irqconn_mtx, TIMESTAMP_US_MAX);
    for (size_t i = 0; i < device->irq_children_len; i++) {
        set_add(&set, (void *)(size_t)device->irq_children[i].irqno);
    }
    mutex_release_shared(&irqconn_mtx);
    return set;
}

// Get a set containing all connected outgoing interrupts.
set_t device_all_out_irq(device_t *device) {
    set_t set = PTR_SET_EMPTY;
    mutex_acquire_shared(&irqconn_mtx, TIMESTAMP_US_MAX);
    for (size_t i = 0; i < device->irq_parents_len; i++) {
        set_add(&set, (void *)(size_t)device->irq_parents[i].irqno);
    }
    mutex_release_shared(&irqconn_mtx);
    return set;
}


// Enable an outgoing interrupt.
errno_t device_enable_irq_out(device_t *device, irqno_t irq_out_pin, bool enabled) {
    mutex_acquire_shared(&device->driver_mtx, TIMESTAMP_US_MAX);
    errno_t res;
    if (!device->driver) {
        res = -EFAULT;
    } else {
        res = device->driver->enable_irq_out(device, irq_out_pin, enabled);
    }
    mutex_release_shared(&device->driver_mtx);
    return res;
}

// Cascade-enable an interrupt output's connected parent pins.
// Does not actually enable this interrupt pin on `device`.
errno_t device_cascade_enable_irq_out(device_t *device, irqno_t out_irqno) {
    irqconns_t        dummy = {.irqno = out_irqno};
    array_binsearch_t index =
        array_binsearch(device->irq_parents, sizeof(irqconns_t), device->irq_parents_len, &dummy, irqconns_irqno_cmp);
    if (!index.found || !device->irq_parents[index.index].connections.len) {
        return -ENOENT;
    }
    dlist_foreach_node(irqconn_t, conn, &device->irq_parents[index.index].connections) {
        device_t *parent = conn->device;
        mutex_acquire_shared(&parent->driver_mtx, TIMESTAMP_US_MAX);
        if (!parent->driver || !parent->driver->cascase_enable_irq) {
            mutex_release_shared(&parent->driver_mtx);
            continue;
        }
        parent->driver->cascase_enable_irq(parent, conn->irqno);
        mutex_release_shared(&parent->driver_mtx);
    }
    return 0;
}

// Enable an incoming interrupt.
errno_t device_enable_irq_in(device_t *device, irqno_t irq_in_pin, bool enabled) {
    mutex_acquire_shared(&device->driver_mtx, TIMESTAMP_US_MAX);
    errno_t res;
    if (!device->driver || !device->driver->enable_irq_in) {
        res = -ENOENT;
    } else {
        res = device->driver->enable_irq_in(device, irq_in_pin, enabled);
    }
    mutex_release_shared(&device->driver_mtx);
    return res;
}

// Send an interrupt to all children on a certain pin.
bool device_forward_interrupt(device_t *device, irqno_t in_irqno) {
    assert_dev_drop(!irq_is_enabled());
    irqconns_t        dummy = {.irqno = in_irqno};
    array_binsearch_t res =
        array_binsearch(device->irq_children, sizeof(irqconns_t), device->irq_children_len, &dummy, irqconns_irqno_cmp);
    if (!res.found) {
        return false;
    }
    bool handled = false;
    dlist_foreach_node(irqconn_t, conn, &device->irq_children[res.index].connections) {
        if (conn->device->driver) {
            handled |= conn->device->driver->interrupt(conn->device, conn->irqno);
        }
    }
    return handled;
}



// Notify of a device interrupt.
void device_interrupt(device_t *device, irqno_t irq_pin) {
    assert_dev_drop(!irq_is_enabled());
    spinlock_take_shared(&irqconn_lock);
    if (device->driver) {
        device->driver->interrupt(device, irq_pin);
    }
    spinlock_release_shared(&irqconn_lock);
}



// Register a new driver.
errno_t driver_add(driver_t const *driver) {
    mutex_acquire(&drivers_mtx, TIMESTAMP_US_MAX);
    if (!set_add(&drivers, driver)) {
        mutex_release(&drivers_mtx);
        return -ENOMEM;
    }
    mutex_release(&drivers_mtx);

    // Try to match driverless devices against this driver.
    mutex_acquire(&devs_mtx, TIMESTAMP_US_MAX);
    map_foreach(ent, &devs_by_id) {
        if (device_add_to_driver(ent->value, driver)) {
            break;
        }
    }
    mutex_release(&devs_mtx);

    return 0;
}

// Remove a driver.
errno_t driver_remove(driver_t const *driver) {
    mutex_acquire(&drivers_mtx, TIMESTAMP_US_MAX);
    if (!set_remove(&drivers, driver)) {
        mutex_release(&drivers_mtx);
        return -ENOENT;
    }
    mutex_release(&drivers_mtx);

    // Remove all devices from this driver.
    map_foreach(ent, &devs_by_id) {
        device_t *dev = ent->value;
        if (dev->driver == driver) {
            device_remove_from_driver(dev);

            // Try to find an alternative driver if possible.
            device_try_find_driver(dev);
        }
    }

    return 0;
}



// Add device nodes to a new devtmpfs.
// Called by the VFS after a devtmpfs filesystem is mounted.
errno_t device_devtmpfs_mounted(file_t devtmpfs_root) {
    RETURN_ON_ERRNO(fs_mkdir(devtmpfs_root, "by_id", 5));

    mutex_acquire_shared(&devs_mtx, TIMESTAMP_US_MAX);
    map_foreach(ent, &devs_by_id) {
        device_t *dev = ent->value;
        char      buf[9];
        npf_snprintf(buf, sizeof(buf), "%08x", (int)dev->id);
        fs_mkdir(devtmpfs_root, buf, 8);

        // TODO: Some infos here maybe?

        mutex_acquire_shared(&dev->driver_mtx, TIMESTAMP_US_MAX);
        if (dev->driver && dev->driver->create_devnodes) {
            file_t devnode_dir = fs_dir_open(devtmpfs_root, buf, 8, 0);
            dev->driver->create_devnodes(dev, devtmpfs_root, devnode_dir);
            fs_dir_close(devnode_dir);
        }
        mutex_release_shared(&dev->driver_mtx);
    }
    mutex_release_shared(&devs_mtx);

    return 0;
}
