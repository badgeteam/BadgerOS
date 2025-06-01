
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
#include "errno.h"
#include "list.h"
#include "memprotect.h"
#include "mutex.h"
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
// Mutex guarding the act of changing the interrupt graph.
static mutex_t          irqconn_mtx  = MUTEX_T_INIT_SHARED;
// Spinlock guarding changes to interrupt graph fields.
static spinlock_t       irqconn_lock = SPINLOCK_T_INIT_SHARED;



// Binary search for device by ID comparator.
static int dev_id_search(void const *a, void const *b) {
    device_union_t const *dev = *(void **)a;
    uint32_t              id  = (size_t)b;
    if (dev->base.id < id) {
        return -1;
    } else if (dev->base.id > id) {
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
    if (match_class && driver->match(&device->base.info) && driver->add(&device->base) >= 0) {
        device->base.dev_class = driver->dev_class;

        // Take the irq spinlock around the driver set to guard against partial write of the field during an interrupt.
        assert_dev_keep(irq_disable());
        spinlock_take(&irqconn_lock);
        device->base.driver = driver;
        spinlock_release(&irqconn_lock);
        irq_enable();

        device_t *parent = device->base.info.parent;
        if (parent && parent->driver && parent->driver->child_got_driver) {
            parent->driver->child_got_driver(parent, &device->base);
        }

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

        device_t *parent = device->base.info.parent;
        if (parent && parent->driver && parent->driver->child_lost_driver) {
            parent->driver->child_lost_driver(parent, &device->base);
        }

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

// Remove a device interrupt link; see `device_link_irq`.
// Reuses `irqconn_t::child.node` to store nodes in `to_free_list`.
static errno_t device_unlink_irq_impl(device_t *child, irqno_t child_pin, device_t *parent, irqno_t parent_pin);

// Register a new device.
// Takes ownership of any memory in `info`, regardless of success.
// Returns a nonzero ID if successful.
device_t *device_add(device_info_t info) {
    device_union_t *device = calloc(1, sizeof(device_union_t));
    if (!device) {
        device_pop_ref(info.parent);
        device_pop_ref(info.irq_parent);
        free(info.addrs);
        return NULL;
    }

    mutex_acquire(&devs_mtx, TIMESTAMP_US_MAX);

    // Insert device into the list.
    if (!array_lencap_insert(&devs, sizeof(void *), &devs_len, &devs_cap, &device, devs_len)) {
        device_pop_ref(info.parent);
        device_pop_ref(info.irq_parent);
        free(info.addrs);
        free(device);
        mutex_release(&devs_mtx);
        return NULL;
    }

    // Initialize device data.
    uint32_t id           = ++id_ctr;
    device->base.info     = info;
    device->base.id       = id;
    device->base.refcount = 2;
    device->base.state    = DEV_STATE_INACTIVE;
    if (!device_init(device)) {
        device_pop_ref(info.parent);
        device_pop_ref(info.irq_parent);
        free(info.addrs);
        free(device);
        mutex_release(&devs_mtx);
        return NULL;
    }

    // Add to parent's set of children.
    device_t *parent = device->base.info.parent;
    if (parent) {
        if (!set_add(parent->children, device)) {
            device_deinit(device);
            free(info.addrs);
            free(device);
            mutex_release(&devs_mtx);
            return NULL;
        }
        if (parent->driver && parent->driver->child_added) {
            errno_t res = parent->driver->child_added(parent, &device->base);
            if (res < 0) {
                set_remove(parent->children, device);
                device_deinit(device);
                free(info.addrs);
                free(device);
                mutex_release(&devs_mtx);
                return NULL;
            }
        }
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
        device->state    = DEV_STATE_ACTIVE;
        device_t *parent = device->info.parent;
        if (parent) {
            if (parent->driver && parent->driver->child_activated) {
                parent->driver->child_activated(parent, device);
            }
        }
    }
    mutex_release(&device->driver_mtx);
    device_try_find_driver((device_union_t *)device);
}

// Remove a device and its children.
// Reuses `irqconn_t::child.node` to store nodes in `to_free_list`.
static uint32_t device_remove_impl(uint32_t id, dlist_t *to_free_list) {
    array_binsearch_t res = array_binsearch(devs, sizeof(void *), devs_len, (void *)(size_t)id, dev_id_search);

    if (res.found) {
        device_union_t *device = devs[res.index];

        // Disconnect from interrupt parents, if any.
        mutex_acquire(&irqconn_mtx, TIMESTAMP_US_MAX);
        for (size_t i = 0; i < device->base.irq_parents_len; i++) {
            while (device->base.irq_parents[i].connections.len) {
                irqconn_t conn = *(irqconn_t *)device->base.irq_parents[i].connections.head;
                device_unlink_irq_impl(&device->base, i, conn.parent.device, conn.parent.irqno);
            }
        }
        mutex_release(&irqconn_mtx);

        // First remove child devices, if any.
        if (device->base.children) {
            set_foreach(device_union_t, child, device->base.children) {
                device_remove_impl(child->base.id, to_free_list);
            }
        }

        if (device->base.driver) {
            // Children removed, remove the device itself.
            device_remove_from_driver(device);
        }

        // Remove from parent's set of children.
        device_t *parent = device->base.info.parent;
        if (parent) {
            if (parent->driver && parent->driver->child_removed) {
                parent->driver->child_removed(parent, &device->base);
            }
            set_remove(parent->children, device);
        }

        // Remove the device from the list.
        array_lencap_remove(&devs, sizeof(void *), &devs_len, &devs_cap, NULL, res.index);
        device_pop_ref((device_t *)device);
    }

    return res.found;
}

// Remove a device and its children.
bool device_remove(uint32_t id) {
    dlist_t to_free_list = DLIST_EMPTY;

    // Recursively remove devices.
    mutex_acquire(&devs_mtx, TIMESTAMP_US_MAX);
    bool success = device_remove_impl(id, &to_free_list);
    mutex_release(&devs_mtx);

    // Free the interrupt connections' memory.
    irqconn_t *conn = container_of(to_free_list.head, irqconn_t, child.node);
    while (conn) {
        irqconn_t *next = container_of(conn->child.node.next, irqconn_t, child.node);
        free(conn);
        conn = next;
    }

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
    if (res.found || *cap >= *len + 1) {
        return 0;
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

    mutex_acquire(&irqconn_mtx, TIMESTAMP_US_MAX);

    // Ensure enough capacity for interrupt connections.
    size_t child_index = RETURN_ON_ERRNO(
        device_alloc_irqno(&child->irq_parents_len, &child->irq_parents_cap, &child->irq_parents, child_pin)
    );
    size_t parent_index = RETURN_ON_ERRNO(
        device_alloc_irqno(&parent->irq_children_len, &parent->irq_children_cap, &parent->irq_children, parent_pin)
    );

    // Allocate a new interrupt connection.
    irqconn_t *conn = ENOMEM_ON_NULL(malloc(sizeof(irqconn_t)));

    conn->child.device  = child;
    conn->child.irqno   = child_pin;
    conn->parent.device = parent;
    conn->parent.irqno  = parent_pin;

    assert_dev_keep(irq_disable());
    spinlock_take(&irqconn_lock);

    // Register the connection to both.
    dlist_append(&child->irq_parents[child_index].connections, &conn->child.node);
    dlist_append(&parent->irq_children[parent_index].connections, &conn->parent.node);

    spinlock_release(&irqconn_lock);
    irq_enable();

    mutex_release(&irqconn_mtx);
    return 0;
}

// Remove a device interrupt link; see `device_link_irq`.
static errno_t device_unlink_irq_impl(device_t *child, irqno_t child_irqno, device_t *parent, irqno_t parent_irqno) {
    // Enfore both devices are in the tree.
    if (child->state == DEV_STATE_REMOVED || parent->state == DEV_STATE_REMOVED) {
        return -EINVAL;
    }

    mutex_acquire(&irqconn_mtx, TIMESTAMP_US_MAX);

    irqconns_t        dummy = {.irqno = child_irqno};
    array_binsearch_t child_idx =
        array_binsearch(child->irq_parents, sizeof(irqconns_t), child->irq_parents_len, &dummy, irqconns_irqno_cmp);
    dummy.irqno = parent_irqno;
    array_binsearch_t parent_idx =
        array_binsearch(child->irq_parents, sizeof(irqconns_t), child->irq_parents_len, &dummy, irqconns_irqno_cmp);

    if (!child_idx.found || !parent_idx.found) {
        mutex_release(&irqconn_mtx);
        return -ENOENT;
    }

    dlist_foreach(irqconn_t, conn, child.node, &child->irq_parents->connections) {
        if (conn->child.irqno == child_irqno && conn->parent.irqno == parent_irqno) {

            assert_dev_keep(irq_disable());
            spinlock_take(&irqconn_lock);

            dlist_remove(&child->irq_parents[child_idx.index].connections, &conn->child.node);
            dlist_remove(&parent->irq_children[parent_idx.index].connections, &conn->parent.node);

            spinlock_release(&irqconn_lock);
            irq_enable();

            mutex_release(&irqconn_mtx);
            free(conn);
            return 0;
        }
    }

    mutex_release(&irqconn_mtx);
    return -ENOENT;
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

// Cascade-enable an interrupt output.
errno_t device_cascade_enable_irq_out(device_t *device, irqno_t out_irqno) {
    mutex_acquire_shared(&device->driver_mtx, TIMESTAMP_US_MAX);
    irqconns_t        dummy = {.irqno = out_irqno};
    array_binsearch_t index =
        array_binsearch(device->irq_parents, sizeof(irqconns_t), device->irq_parents_len, &dummy, irqconns_irqno_cmp);
    if (!index.found) {
        return -ENOENT;
    }
    if (!device->driver || !device->irq_parents[index.index].connections.len) {
        mutex_release_shared(&device->driver_mtx);
        return -ENOENT;
    }
    errno_t res = device->driver->enable_irq_out(device, out_irqno, true);
    if (res < 0) {
        return res;
    }
    dlist_foreach(irqconn_t, conn, child.node, &device->irq_parents[index.index].connections) {
        device_t *parent = conn->parent.device;
        if (!parent->driver || !parent->driver->cascase_enable_irq) {
            continue;
        }
        parent->driver->cascase_enable_irq(parent, conn->parent.irqno);
    }
    mutex_release_shared(&device->driver_mtx);
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
    dlist_foreach(irqconn_t, conn, parent.node, &device->irq_children[res.index].connections) {
        if (conn->child.device->driver) {
            handled |= conn->child.device->driver->interrupt(conn->child.device, conn->child.irqno);
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
    for (size_t i = 0; i < devs_len; i++) {
        if (device_add_to_driver(devs[i], driver)) {
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
    for (size_t i = 0; i < devs_len; i++) {
        if (devs[i]->base.driver == driver) {
            device_remove_from_driver(devs[i]);

            // Try to find an alternative driver if possible.
            device_try_find_driver(devs[i]);
        }
    }

    return 0;
}



// Add device nodes to a new devtmpfs.
// Called by the devtmpfs mount function.
void device_devtmpfs_mounted(file_t devtmpfs_root) {
}
