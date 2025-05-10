
// SPDX-FileCopyrightText: 2025 Julian Scheffers <julian@scheffers.net>
// SPDX-FileType: SOURCE
// SPDX-License-Identifier: MIT

#include "cpu/driver/riscv_intc.h"

#include "badge_strings.h"
#include "cpu/isr_ctx.h"
#include "cpu/regs.h"
#include "cpu/riscv.h"
#include "cpulocal.h"
#include "device/class/irqctl.h"
#include "device/device.h"
#include "log.h"
#include "panic.h"

#ifdef CPU_RISCV_ENABLE_SBI_TIME
// Called by the interrupt handler when the CPU-local timer fires.
void riscv_sbi_timer_interrupt();
#endif

bool riscv_intc_match(device_info_t *info) {
    return device_test_dtb_compat(info, 1, (char const *const[]){"riscv,cpu-intc"});
}

bool riscv_intc_add(device_t *device) {
    asm("csrwi sie, 0");
    return true;
}

void riscv_intc_remove(device_t *device) {
}

void riscv_intc_interrupt(device_t *device, irqpin_t pin) {
    device_irqctl_t *irqctl = (void *)device;
    if (pin == RISCV_INT_SUPERVISOR_EXT && irqctl->irq_children[pin].len) {
        device_irqctl_forward_interrupt((device_irqctl_t *)device, pin);
#ifdef CPU_RISCV_ENABLE_SBI_TIME
    } else if (pin == RISCV_INT_SUPERVISOR_TIMER) {
        asm("csrc sie, %0" ::"r"(1 << RISCV_INT_SUPERVISOR_TIMER));
        riscv_sbi_timer_interrupt();
#endif
    } else {
        logkf_from_isr(LOG_FATAL, "Unhandled interrupt 0x%{size;x}", pin);
        panic_abort();
    }
}

bool riscv_intc_enable_irq(device_t *device, irqpin_t pin, bool enable) {
    return false;
}

bool riscv_intc_enable_in(device_irqctl_t *device, irqpin_t pin, bool enable) {
    if (pin == 0 || pin > 32) {
        return false;
    }
    if (enable) {
        asm("csrs sie, %0" ::"r"(1 << pin));
    } else {
        asm("csrc sie, %0" ::"r"(1 << pin));
    }
    return true;
}


driver_irqctl_t const riscv_intc_driver = {
    .base.dev_class  = DEV_CLASS_IRQCTL,
    .base.match      = riscv_intc_match,
    .base.add        = riscv_intc_add,
    .base.remove     = riscv_intc_remove,
    .base.interrupt  = riscv_intc_interrupt,
    .base.enable_irq = riscv_intc_enable_irq,
    .enable_in       = riscv_intc_enable_in,
};
