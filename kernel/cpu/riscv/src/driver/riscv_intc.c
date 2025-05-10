
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

bool riscv_intc_match(device_t *device) {
    for (size_t i = 0; i < device->compat_count; i++) {
        if (cstr_equals(device->compats[i], "riscv,cpu-intc")) {
            return true;
        }
    }
    return false;
}

void riscv_intc_add(device_t *device) {
    asm("csrwi sie, 0");
}

void riscv_intc_remove(device_t *device) {
}

void riscv_intc_interrupt(device_t *device, size_t int_no) {
    device_irqctl_t *irqctl = (void *)device;
    if (int_no == RISCV_INT_SUPERVISOR_EXT && irqctl->irq_children[int_no].device) {
        device_interrupt(irqctl->irq_children[int_no].device, irqctl->irq_children[int_no].pin);
#ifdef CPU_RISCV_ENABLE_SBI_TIME
    } else if (int_no == RISCV_INT_SUPERVISOR_TIMER) {
        asm("csrc sie, %0" ::"r"(1 << RISCV_INT_SUPERVISOR_TIMER));
        riscv_sbi_timer_interrupt();
#endif
    } else {
        logkf_from_isr(LOG_FATAL, "Unhandled interrupt 0x%{size;x}", int_no);
        panic_abort();
    }
}

bool riscv_intc_enable_irq(device_t *device, size_t pin, bool enable) {
    return false;
}

bool riscv_intc_enable_in(device_t *device, size_t pin, bool enable) {
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
