
// SPDX-License-Identifier: MIT

#pragma once

#include "cpu/riscv.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>



// Enable supervisor access to user memory.
static inline void mmu_enable_sum() {
    asm("csrs sstatus, %0" ::"r"((1 << RISCV_STATUS_SUM_BIT) | (1 << RISCV_STATUS_MXR_BIT)));
}
// Disable supervisor access to user memory.
static inline void mmu_disable_sum() {
    asm("csrc sstatus, %0" ::"r"((1 << RISCV_STATUS_SUM_BIT) | (1 << RISCV_STATUS_MXR_BIT)));
}



// Notify the MMU of global mapping changes.
static inline void mmu_vmem_fence() {
    asm volatile("fence rw,rw" ::: "memory");
    asm volatile("sfence.vma" ::: "memory");
}
