
// SPDX-FileCopyrightText: 2025 Julian Scheffers <julian@scheffers.net>
// SPDX-FileType: SOURCE
// SPDX-License-Identifier: MIT

#include "cpu/driver/riscv_intc.h"

#include "cpu/riscv.h"
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

errno_t riscv_intc_add(device_t *device) {
    asm("csrwi sie, 0");
    return 0;
}

void riscv_intc_remove(device_t *device) {
}

bool riscv_intc_interrupt(device_t *device, irqno_t pin) {
    device_irqctl_t *irqctl = (void *)device;
#ifdef CPU_RISCV_ENABLE_SBI_TIME
    if (pin == RISCV_INT_TIMER) {
        asm("csrc sie, %0" ::"r"(1 << RISCV_INT_TIMER));
        riscv_sbi_timer_interrupt();
    } else
#endif
        if (!device_forward_interrupt(device, pin)) {
        logkf_from_isr(LOG_FATAL, "Unhandled interrupt 0x%{size;x}", pin);
        panic_abort();
    }
    return true;
}

errno_t riscv_intc_enable_irq(device_t *device, irqno_t pin, bool enable) {
    return -ENOTSUP;
}

errno_t riscv_intc_enable_in(device_t *device, irqno_t pin, bool enable) {
    if (pin == 0 || pin > 32) {
        return -EINVAL;
    }
    if (enable) {
        asm("csrs sie, %0" ::"r"(1 << pin));
    } else {
        asm("csrc sie, %0" ::"r"(1 << pin));
    }
    return 0;
}

static errno_t riscv_intc_cascade_enable(device_t *device, irqno_t irq_in_pin) {
    riscv_intc_enable_in(device, irq_in_pin, true);
    return 0;
}



driver_irqctl_t const riscv_intc_driver = {
    .base.dev_class          = DEV_CLASS_IRQCTL,
    .base.match              = riscv_intc_match,
    .base.add                = riscv_intc_add,
    .base.remove             = riscv_intc_remove,
    .base.interrupt          = riscv_intc_interrupt,
    .base.enable_irq_out     = riscv_intc_enable_irq,
    .base.enable_irq_in      = riscv_intc_enable_in,
    .base.cascase_enable_irq = riscv_intc_cascade_enable,
};
