
// SPDX-License-Identifier: MIT

#pragma once

#include "cpu/regs.h"
#include "cpu/riscv.h"
#include "isr_ctx.h"

#ifndef __ASSEMBLER__
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#endif

#ifdef __ASSEMBLER__
// clang-format off

// Interrupt vector table implemented in ASM.
    .global riscv_interrupt_vector_table
// Called from ASM on interrupt.
    .global riscv_interrupt_handler
// Called from ASM on trap.
    .global riscv_trap_handler

// clang-format on
#else
// Interrupt vector table implemented in ASM.
extern uint32_t const riscv_interrupt_vector_table[32];
// Callback from ASM to platform-specific interrupt handler.
extern void           riscv_interrupt_handler();
// ASM system call wrapper function.
extern void           riscv_syscall_wrapper();
// Callback from ASM on non-syscall trap.
extern void           riscv_trap_handler();

// Explicit context switch from kernel.
// Interrupts must be disabled on entry and will be re-enabled on exit.
// If the context switch target is not set, this is a NOP.
extern void        isr_context_switch();
// Pause the CPU briefly.
static inline void isr_pause() {
    // RISC-V Zihintpause instruction.
    // This is a fence with PRED=W and SUCC=none.
    asm(".word 0x0100000f");
}

#endif
