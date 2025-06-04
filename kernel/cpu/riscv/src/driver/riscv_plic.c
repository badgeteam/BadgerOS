
// SPDX-FileCopyrightText: 2025 Julian Scheffers <julian@scheffers.net>
// SPDX-FileType: SOURCE
// SPDX-License-Identifier: MIT

#include "cpu/driver/riscv_plic.h"

#include "assertions.h"
#include "cpu/riscv.h"
#include "device/class/irqctl.h"
#include "device/dev_addr.h"
#include "device/device.h"
#include "device/dtb/dtb.h"
#include "errno.h"
#include "log.h"
#include "malloc.h"
#include "panic.h"
#include "set.h"
#include "smp.h"

#define REG_READ(addr)       (*(uint32_t const volatile *)(addr))
#define REG_WRITE(addr, val) (*(uint32_t volatile *)(addr) = (val))
#define REG_SET_BIT(addr, bitno)                                                                                       \
    ({                                                                                                                 \
        size_t   atmp = (addr);                                                                                        \
        uint32_t mask = 1ul << (bitno);                                                                                \
        REG_WRITE(atmp, REG_READ(atmp) | mask);                                                                        \
    })
#define REG_CLEAR_BIT(addr, bitno)                                                                                     \
    ({                                                                                                                 \
        size_t   atmp = (addr);                                                                                        \
        uint32_t mask = 1ul << (bitno);                                                                                \
        REG_WRITE(atmp, REG_READ(atmp) & ~mask);                                                                       \
    })

static bool    riscv_plic_match(device_info_t *device);
static errno_t riscv_plic_add(device_t *device);
static void    riscv_plic_remove(device_t *device);
static bool    riscv_plic_interrupt(device_t *device, irqno_t pin);
static errno_t riscv_plic_enable_irq_out(device_t *device, irqno_t pin, bool enable);
static errno_t riscv_plic_enable_irq_in(device_t *device, irqno_t pin, bool enable);



static bool riscv_plic_match(device_info_t *info) {
    return device_test_dtb_compat(info, 1, (char const *const[]){"riscv,plic0", "sifive,plic-1.0.0"});
}

static errno_t riscv_plic_add(device_t *device) {
    // PLIC must have one MMIO address.
    if (device->info.addrs_len != 1 || device->info.addrs[0].type != DEV_ATYPE_MMIO) {
        return -EINVAL;
    }

    // Allocate cookie.
    device->cookie = calloc(1, sizeof(device_riscv_plic_t));
    if (!device->cookie) {
        return -ENOMEM;
    }
    device_riscv_plic_t *cookie = device->cookie;

    assert_always(dtb_read_uint(device->info.dtb_handle, device->info.dtb_node, "#interrupt-cells") == 1);
    cookie->ndev = dtb_read_uint(device->info.dtb_handle, device->info.dtb_node, "riscv,ndev");

    // Select appropriate PLIC context to use per CPU.
    cookie->ctx_by_cpu = calloc(smp_count, sizeof(int));
    if (!cookie->ctx_by_cpu) {
        free(cookie);
        return -ENOMEM;
    }
    // Initialize all to -1 (invalid).
    for (int i = 0; i < smp_count; i++) {
        cookie->ctx_by_cpu[i] = -1;
    }

    // For each outgoing interrupt (PLIC context)
    set_t all_out = device_all_out_irq(device);
    set_foreach(void, ctx_irqno, &all_out) {
        devirqno_arr_t outs = device_list_out_irq(device, (size_t)ctx_irqno);
        for (size_t j = 0; j < outs.len; j++) {
            device_t *parent       = outs.arr[j].device;
            irqno_t   parent_irqno = outs.arr[j].irqno;
            if (parent_irqno == RISCV_INT_EXT) {
                // Get CPU ID from DTB parent node.
                dtb_node_t *cpu_node = parent->info.dtb_node ? parent->info.dtb_node->parent : NULL;
                if (!cpu_node)
                    continue;
                uint32_t cpu_id      = dtb_read_uint(parent->info.dtb_handle, cpu_node, "reg");
                int      logical_cpu = smp_get_cpu(cpu_id);
                if (logical_cpu >= 0 && logical_cpu < smp_count) {
                    cookie->ctx_by_cpu[logical_cpu] = (size_t)ctx_irqno;
                }
            }
        }
        devirqno_arr_free(outs);
    }
    set_clear(&all_out);

    // Ensure every logical CPU has a valid PLIC context.
    for (int i = 0; i < smp_count; i++) {
        if (cookie->ctx_by_cpu[i] == -1) {
            logkf(LOG_FATAL, "CPU%{d} missing PLIC context", i);
            panic_abort();
        }
    }

    // Disable all interrupts.
    for (size_t i = 0; i < device->irq_children_len; i++) {
        riscv_plic_enable_irq_in(device, i, false);
    }
    return 0;
}

static void riscv_plic_remove(device_t *device) {
}

static bool riscv_plic_interrupt(device_t *device, irqno_t pin) {
    device_riscv_plic_t *plic   = device->cookie;
    size_t               vaddr  = device->info.addrs[0].mmio.vaddr;
    uint32_t             ctx_no = plic->ctx_by_cpu[smp_cur_cpu()];
    uint32_t             irq    = REG_READ(PLIC_CLAIM_OFF(ctx_no) + vaddr);
    if (irq) {
        // Interrupt claimed by this CPU.
        return device_forward_interrupt(device, irq);
    } else {
        // Interrupt presumed to have been handled by another CPU.
        return true;
    }
}

static errno_t riscv_plic_enable_irq_out(device_t *device, irqno_t pin, bool enable) {
    return -ENOTSUP;
}

static errno_t riscv_plic_enable_irq_in(device_t *device, irqno_t pin, bool enable) {
    size_t               vaddr = device->info.addrs[0].mmio.vaddr;
    device_riscv_plic_t *plic  = device->cookie;
    for (int i = 0; i < smp_count; i++) {
        uint32_t ctx_no = plic->ctx_by_cpu[i];
        if (enable) {
            REG_SET_BIT(PLIC_ENABLE_OFF(ctx_no) + pin / 32 * 4 + vaddr, pin % 32);
        } else {
            REG_CLEAR_BIT(PLIC_ENABLE_OFF(ctx_no) + pin / 32 * 4 + vaddr, pin % 32);
        }
    }
    return 0;
}

static errno_t riscv_plic_cascade_enable(device_t *device, irqno_t irq_in_pin) {
    device_riscv_plic_t *plic = device->cookie;

    // Enable the input IRQ.
    RETURN_ON_ERRNO(riscv_plic_enable_irq_in(device, irq_in_pin, true));

    for (int cpu = 0; cpu < smp_count; cpu++) {
        int ctx_no = plic->ctx_by_cpu[cpu];

        // Find the parent interrupt controller for this context
        devirqno_arr_t outs = device_list_out_irq(device, ctx_no);
        for (size_t j = 0; j < outs.len; j++) {
            device_t *parent       = outs.arr[j].device;
            irqno_t   parent_irqno = outs.arr[j].irqno;
            if (parent->dev_class == DEV_CLASS_IRQCTL && parent_irqno == RISCV_INT_EXT) {
                // Enable the external interrupt on the parent controller
                errno_t res = device_enable_irq_in(parent, parent_irqno, true);
                if (res < 0) {
                    devirqno_arr_free(outs);
                    return res;
                }
            }
        }
        devirqno_arr_free(outs);
    }

    return 0;
}



driver_irqctl_t const riscv_plic_driver = {
    .base.dev_class          = DEV_CLASS_IRQCTL,
    .base.match              = riscv_plic_match,
    .base.add                = riscv_plic_add,
    .base.remove             = riscv_plic_remove,
    .base.interrupt          = riscv_plic_interrupt,
    .base.enable_irq_out     = riscv_plic_enable_irq_out,
    .base.enable_irq_in      = riscv_plic_enable_irq_in,
    .base.cascase_enable_irq = riscv_plic_cascade_enable,
};
