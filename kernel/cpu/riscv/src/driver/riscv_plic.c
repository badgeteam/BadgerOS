
// SPDX-FileCopyrightText: 2025 Julian Scheffers <julian@scheffers.net>
// SPDX-FileType: SOURCE
// SPDX-License-Identifier: MIT

#include "cpu/driver/riscv_plic.h"

#include "cpu/isr_ctx.h"
#include "cpu/regs.h"
#include "cpu/riscv.h"
#include "cpulocal.h"
#include "device/class/irqctl.h"
#include "device/dev_addr.h"
#include "device/device.h"
#include "log.h"
#include "panic.h"

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

static bool riscv_plic_match(device_info_t *device);
static bool riscv_plic_add(device_t *device);
static void riscv_plic_remove(device_t *device);
static void riscv_plic_interrupt(device_t *device, irqpin_t pin);
static bool riscv_plic_enable_irq(device_t *device, irqpin_t pin, bool enable);
static bool riscv_plic_enable_in(device_irqctl_t *device, irqpin_t pin, bool enable);



static bool riscv_plic_match(device_info_t *info) {
    return device_test_dtb_compat(info, 1, (char const *const[]){"riscv,plic0", "sifive,plic-1.0.0"});
}

static bool riscv_plic_add(device_t *device) {
    if (device->info.addrs_len != 1 || device->info.addrs[0].type != DEV_ATYPE_MMIO) {
        return false;
    }
    device_riscv_plic_t *plic = (void *)device;
    for (size_t i = 0; i < plic->base.incoming_len; i++) {
        riscv_plic_enable_in((device_irqctl_t *)device, i, false);
    }
    return true;
}

static void riscv_plic_remove(device_t *device) {
}

static void riscv_plic_interrupt(device_t *device, irqpin_t pin) {
    device_riscv_plic_t *plic  = (void *)device;
    size_t               vaddr = device->info.addrs[0].mmio.vaddr;
    uint32_t             irq   = REG_READ(PLIC_CLAIM_OFF(plic->ctx_no) + vaddr);
    if (irq) {
        device_irqctl_forward_interrupt((device_irqctl_t *)device, irq);
    }
}

static bool riscv_plic_enable_irq(device_t *device, irqpin_t pin, bool enable) {
    return false;
}

static bool riscv_plic_enable_in(device_irqctl_t *device, irqpin_t pin, bool enable) {
    device_riscv_plic_t *plic  = (void *)device;
    size_t               vaddr = device->base.info.addrs[0].mmio.vaddr;
    if (enable) {
        REG_SET_BIT(PLIC_ENABLE_OFF(plic->ctx_no) + pin / 32 * 4 + vaddr, pin % 32);
    } else {
        REG_CLEAR_BIT(PLIC_ENABLE_OFF(plic->ctx_no) + pin / 32 * 4 + vaddr, pin % 32);
    }
    return false;
}


driver_irqctl_t const riscv_plic_driver = {
    .base.dev_class  = DEV_CLASS_IRQCTL,
    .base.match      = riscv_plic_match,
    .base.add        = riscv_plic_add,
    .base.remove     = riscv_plic_remove,
    .base.interrupt  = riscv_plic_interrupt,
    .base.enable_irq = riscv_plic_enable_irq,
    .enable_in       = riscv_plic_enable_in,
};
