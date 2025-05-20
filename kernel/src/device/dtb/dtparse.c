
// SPDX-License-Identifier: MIT

#include "device/dtb/dtparse.h"

#include "arrays.h"
#include "assertions.h"
#include "device/dev_addr.h"
#include "device/dev_class.h"
#include "device/device.h"
#include "device/dtb/dtb.h"
#include "log.h"
#include "rawprint.h"
#include "smp.h"

#if __BYTE_ORDER == __LITTLE_ENDIAN
#define htobe32(x) __builtin_bswap32(x)
#define be32toh(x) __builtin_bswap32(x)
#else
#define htobe32(x) (x)
#define be32toh(x) (x)
#endif



// Determine what type of address is expected.
static bool determine_atype(device_t *parent_device, dev_atype_t *atype) {
    *atype = DEV_ATYPE_MMIO;
    while (parent_device) {
        switch (parent_device->dev_class) {
            case DEV_CLASS_UNKNOWN: // Unknown device, unknown address.
            case DEV_CLASS_BLOCK:   // Has no standard child address format.
            case DEV_CLASS_IRQCTL:  // Has no standard child address format.
            case DEV_CLASS_TTY:     // Has no standard child address format.
            case DEV_CLASS_AHCI:    // Should not be in the DTB.
                return false;

            case DEV_CLASS_PCICTL: *atype = DEV_ATYPE_PCI; return true;
            case DEV_CLASS_I2CCTL: *atype = DEV_ATYPE_I2C; return true;
        }
    }
    return atype;
}

// Try to extract the address from a DTB node.
static void get_node_addr(
    dtb_handle_t *handle, dtb_node_t *node, uint32_t alen, uint32_t slen, device_t *parent_device, device_info_t *info
) {
    dev_atype_t atype;
    if (!determine_atype(parent_device, &atype)) {
        return;
    }

    dtb_prop_t *reg = dtb_get_prop(handle, node, "reg");
    if (!reg) {
        return;
    }

    size_t n_ents = reg->content_len / 4 / (alen + slen);
    if (reg->content_len != 4 * (alen + slen)) {
        return;
    }

    switch (atype) {
        default: __builtin_unreachable();
        case DEV_ATYPE_AHCI: // Cannot be parsed from DTB.
        case DEV_ATYPE_PCI:  // Cannot be parsed from DTB.
            return;

        case DEV_ATYPE_MMIO:
            for (size_t i = 0; i < n_ents; i++) {
                dev_addr_t ent = {
                    .type       = DEV_ATYPE_MMIO,
                    .mmio.paddr = dtb_prop_read_cells(handle, reg, i * (alen + slen), alen),
                    .mmio.size  = dtb_prop_read_cells(handle, reg, i * (alen + slen) + alen, slen),
                };
                array_len_insert(&info->addrs, sizeof(dev_addr_t), &info->addrs_len, &ent, info->addrs_len);
            }
            return;

        case DEV_ATYPE_I2C:
            if (alen != 1 || slen != 0) {
                return;
            }
            for (size_t i = 0; i < n_ents; i++) {
                dev_addr_t ent = {
                    .type = DEV_ATYPE_I2C,
                    .i2c  = dtb_prop_read_cells(handle, reg, i, 1),
                };
                array_len_insert(&info->addrs, sizeof(dev_addr_t), &info->addrs_len, &ent, info->addrs_len);
            }
            return;
    }
}

// Recursive DTB parsing imlpementation that walks the SOC for devices and registers them all.
static device_t *
    dtparse_impl(dtb_handle_t *handle, dtb_node_t *node, uint32_t alen, uint32_t slen, device_t *parent_device) {
    device_t *device = NULL;

    if (dtb_get_prop(handle, node, "compatible")) {
        // If a compatible node is present, register it as a device.
        device_info_t info = {
            .parent     = parent_device,
            .dtb_handle = handle,
            .dtb_node   = node,
        };
        get_node_addr(handle, node, alen, slen, parent_device, &info);
        device = device_add(info);
        // TODO: Establish interrupt connections.
        device_activate(device);
    }

    // Walk child nodes.
    uint32_t inner_alen = dtb_read_uint(handle, node, "#address-cells");
    uint32_t inner_slen = dtb_read_uint(handle, node, "#size-cells");
    for (dtb_node_t *child = node->nodes; child; child = child->next) {
        device_t *child_dev =
            dtparse_impl(handle, child, inner_alen ?: alen, inner_slen ?: slen, device ?: parent_device);
        if (child_dev) {
            device_pop_ref(child_dev);
        }
    }

    return device;
}

// Parse the DTB and add found devices.
void dtparse(void *dtb_ptr) {
    // Open the DTB for reading.
    dtb_handle_t *handle = dtb_open(dtb_ptr);
    assert_always(handle);
    dtb_node_t *root = dtb_root_node(handle);

    // The SOC node contains devices for which we may have drivers.
    dtb_node_t *cpus     = dtb_get_node(handle, root, "cpus");
    dtb_node_t *soc      = dtb_get_node(handle, root, "soc");
    uint32_t    soc_alen = dtb_read_uint(handle, soc, "#address-cells");
    uint32_t    soc_slen = dtb_read_uint(handle, soc, "#size-cells");

    // Initialise timers.
    time_init_dtb(handle);
    // Initialise SMP.
    smp_init_dtb(handle);

    // Walk the CPU nodes to find CPU root interrupt controllers.
    for (dtb_node_t *cpu = cpus->nodes; cpu; cpu = cpu->next) {
        dtb_prop_t *reg = dtb_get_prop(handle, cpu, "reg");
        if (!reg) {
            continue;
        }
        int smp_idx = smp_get_cpu(dtb_prop_read_uint(handle, reg));
        if (smp_idx == -1) {
            continue;
        }
        dtb_node_t *irqctl_node = dtb_get_node(handle, cpu, "interrupt-controller");
        if (!irqctl_node) {
            logkf(LOG_ERROR, "Missing interrupt controller for CPU%{d}", smp_idx);
            continue;
        }
        device_t *irqctl = dtparse_impl(handle, irqctl_node, 0, 0, NULL);
        if (!irqctl || !irqctl->driver || irqctl->dev_class != DEV_CLASS_IRQCTL) {
            logkf(LOG_ERROR, "Invalid interrupt controller for CPU%{d}", smp_idx);
        } else {
            smp_get_cpulocal(smp_idx)->root_irqctl = irqctl;
        }
    }

    // Walk the SOC node to detect devices and install drivers.
    for (dtb_node_t *node = soc->nodes; node; node = node->next) {
        device_t *child_dev = dtparse_impl(handle, node, soc_alen, soc_slen, NULL);
        if (child_dev) {
            device_pop_ref(child_dev);
        }
    }
}



static void pindent(size_t count) {
    while (count--) {
        rawputc(' ');
        rawputc(' ');
    }
}

static bool isbin(uint8_t const *mem, uint32_t len) {
    if (mem[0] == 0 || mem[len - 1] != 0) {
        return true;
    }
    while (len--) {
        if (*mem && (*mem < 0x20 || *mem > 0x7e)) {
            return true;
        }
        mem++;
    }
    return false;
}

static void escprint(char const *str, uint32_t len) {
    while (len--) {
        if (len == 0 && *str == 0) {
            return;
        } else if (*str == 0) {
            rawputc('\\');
            rawputc('0');
        } else {
            rawputc(*str);
        }
        str++;
    }
}

static void hexprint4(uint32_t const *bin, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) {
        rawprint("0x");
        rawprinthex(be32toh(bin[i]), 8);
        if (i < len - 1) {
            rawputc(' ');
        }
    }
}

static void hexprint1(uint8_t const *bin, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) {
        rawprint("0x");
        rawprinthex(bin[i], 2);
        if (i < len - 1) {
            rawputc(' ');
        }
    }
}

static void dtdump_r(dtb_handle_t *handle, dtb_node_t *node) {
    pindent(node->depth);
    rawprint(node->name);
    rawprint(" {\n");

    dtb_prop_t *prop = node->props;
    while (prop) {
        pindent(node->depth + 1);
        rawprint(prop->name);
        if (prop->content_len) {
            rawprint(" = ");
            if (isbin((uint8_t *)prop->content, prop->content_len)) {
                rawputc('<');
                if (prop->content_len % 4) {
                    hexprint1((uint8_t *)prop->content, prop->content_len);
                } else {
                    hexprint4((uint32_t *)prop->content, prop->content_len / 4);
                }
                rawputc('>');
            } else {
                rawputc('"');
                escprint((char *)prop->content, prop->content_len);
                rawputc('"');
            }
        }
        rawprint(";\n");
        prop = prop->next;
    }

    dtb_node_t *subnode = node->nodes;
    while (subnode) {
        dtdump_r(handle, subnode);
        subnode = subnode->next;
    }

    pindent(node->depth);
    rawprint("}\n");
}

// Dump the DTB.
void dtdump(void *dtb_ptr) {
    dtb_handle_t *handle = dtb_open(dtb_ptr);
    if (!handle) {
        return;
    }
    dtb_node_t *root = dtb_root_node(handle);
    if (!root) {
        rawprint("Invalid root node\n");
        return;
    }
    dtdump_r(handle, root);
}
