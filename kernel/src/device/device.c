
// SPDX-FileCopyrightText: 2025 Julian Scheffers <julian@scheffers.net>
// SPDX-FileType: SOURCE
// SPDX-License-Identifier: MIT

#include "device/device.h"

#include "arrays.h"
#include "assertions.h"
#include "device/dev_addr.h"
#include "mutex.h"
#include "set.h"
#include "time.h"

#include <stddef.h>



// ID -> device map.
static size_t     devs_len, devs_cap;
// ID -> device map.
static device_t **devs;
// Device ID counter.
static uint32_t   id_ctr;
// Devices list mutex.
static mutex_t    devs_mtx = MUTEX_T_INIT_SHARED;
// Set of drivers.
static set_t      drivers  = PTR_SET_EMPTY;



// Binary search for device by ID comparator.
static int dev_id_search(void const *a, void const *b) {
    device_t const *dev = a;
    uint32_t        id  = (size_t)b;
    if (dev->id < id) {
        return -1;
    } else if (dev->id > id) {
        return 1;
    } else {
        return 0;
    }
}



// Register a new device.
// The `driver` field may be NULL and the `children` field must be NULL.
// Updates the device's ID if successful.
bool device_register(device_t *device) {
    assert_always(device->children == NULL);
    assert_always(device->id == 0);
    mutex_acquire(&devs_mtx, TIMESTAMP_US_MAX);

    // Try to insert the device in to the big ol' list.
    bool success = array_lencap_insert(&devs, sizeof(void *), &devs_len, &devs_cap, &device, devs_len);
    if (success) {
        device->id = ++id_ctr;

        // Search for matching device drivers.
        if (device->driver) {
            assert_always(set_contains(&drivers, device->driver));
            device->driver->add(device);
        } else {
            set_foreach(driver_t const, driver, &drivers) {
                if (driver->match(device)) {
                    device->driver = driver;
                    driver->add(device);
                }
            }
        }
    }

    mutex_release(&devs_mtx);
    return success;
}

// Remove a device and its children.
static uint32_t device_remove_impl(uint32_t id) {
    array_binsearch_t res = array_binsearch(devs, sizeof(void *), devs_len, (void *)(size_t)id, dev_id_search);

    if (res.found) {
        device_t *device = devs[res.index];

        // First remove child devices, if any.
        if (device->children) {
            set_foreach(device_t, child, device->children) {
                device_remove_impl(child->id);
            }
        }

        // Children removed, remove the device itself.
        if (device->driver) {
            device->driver->remove(device);
        }

        // Remove the device from the list.
        array_lencap_remove(&devs, sizeof(void *), &devs_len, &devs_cap, NULL, res.index);
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



// Register a new driver.
bool driver_add(driver_t const *driver) {
    if (!set_add(&drivers, driver)) {
        return false;
    }

    // Try to match driverless devices against this driver.
    mutex_acquire(&devs_mtx, TIMESTAMP_US_MAX);
    for (size_t i = 0; i < devs_len; i++) {
        if (devs[i]->driver == NULL && driver->match(devs[i])) {
            devs[i]->driver = driver;
            driver->add(devs[i]);
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
        if (devs[i]->driver == driver) {
            driver->remove(devs[i]);
            devs[i]->driver = NULL;

            // Try to find an alternative driver if possible.
            set_foreach(driver_t const, alternative, &drivers) {
                if (alternative->match(devs[i])) {
                    devs[i]->driver = alternative;
                    alternative->add(devs[i]);
                    break;
                }
            }
        }
    }

    return true;
}
