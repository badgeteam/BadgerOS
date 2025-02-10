
// SPDX-License-Identifier: MIT

#include "cpu/isr.h"

#include "assertions.h"
#include "backtrace.h"
#include "cpu/isr_ctx.h"
#include "interrupt.h"
#include "log.h"
#include "panic.h"
#include "port/hardware.h"
#include "process/internal.h"
#include "process/sighandler.h"
#include "process/types.h"
#include "rawprint.h"
#include "scheduler/cpu.h"
#include "scheduler/types.h"
#if MEMMAP_VMEM
#include "cpu/mmu.h"
#include "memprotect.h"
#endif



// Table of trap names.
static char const *const trapnames[] = {
    "Instruction address misaligned",
    "Instruction access fault",
    "Illegal instruction",
    "Breakpoint",
    "Load address misaligned",
    "Load access fault",
    "Store address misaligned",
    "Store access fault",
    NULL, // "ECALL from U-mode",
    "ECALL from S-mode",
    NULL, // Reserved
    "ECALL from M-mode",
    "Instruction page fault",
    "Load page fault",
    NULL, // Reserved
    "Store page fault",
};
enum { TRAPNAMES_LEN = sizeof(trapnames) / sizeof(trapnames[0]) };

// Bitmask of traps that have associated memory addresses.
#define MEM_ADDR_TRAPS                                                                                                 \
    ((1 << RISCV_TRAP_LACCESS) | (1 << RISCV_TRAP_SACCESS) | (1 << RISCV_TRAP_IACCESS) | (1 << RISCV_TRAP_LPAGE) |     \
     (1 << RISCV_TRAP_SPAGE) | (1 << RISCV_TRAP_IPAGE))

// Kill a process from a trap / ISR.
static void kill_proc_on_trap() {
    proc_exit_self(-1);
    irq_disable();
    sched_lower_from_isr();
    isr_context_switch();
    assert_unreachable();
}

// Called from ASM on non-system call trap.
void riscv_trap_handler() {
    // Redirect interrupts to the interrupt handler.
    long cause, status, tval, epc;
    asm volatile("csrr %0, " CSR_CAUSE_STR : "=r"(cause));
    long trapno = cause & RISCV_VT_ICAUSE_MASK;
    if (cause < 0) {
        riscv_interrupt_handler();
        return;
    }
    asm volatile("csrr %0, " CSR_TVAL_STR : "=r"(tval));
    asm volatile("csrr %0, " CSR_STATUS_STR : "=r"(status));

    isr_ctx_t recurse_ctx;
    recurse_ctx.mpu_ctx  = NULL;
    recurse_ctx.flags    = ISR_CTX_FLAG_IN_ISR | ISR_CTX_FLAG_KERNEL;
    isr_ctx_t *kctx      = isr_ctx_swap(&recurse_ctx);
    recurse_ctx.thread   = kctx->thread;
    recurse_ctx.cpulocal = kctx->cpulocal;

    // Double fault detection.
    bool fault3 = kctx->flags & ISR_CTX_FLAG_2FAULT;
    bool fault2 = kctx->flags & ISR_CTX_FLAG_IN_ISR;
    if (fault2) {
        kctx->flags |= ISR_CTX_FLAG_2FAULT;
    }

    // Check for custom trap handler.
    if (!fault2 && (kctx->flags & ISR_CTX_FLAG_NOEXC) && kctx->noexc_cb(kctx, kctx->noexc_cookie)) {
        isr_ctx_swap(kctx);
        return;
    }

    if (!fault2 && !(kctx->flags & ISR_CTX_FLAG_KERNEL)) {
        switch (trapno) {
                // Unknown trap? The kernel must have messed up, don't handle it.
            default: break;

            case RISCV_TRAP_U_ECALL:
                // ECALL from U-mode goes to system call handler.
                sched_raise_from_isr(kctx->thread, true, riscv_syscall_wrapper);
                isr_ctx_swap(kctx);
                return;

#ifndef MEMMAP_VMEM
                // Access faults should never happen with VMEM enabled; this is the kernel's failing.
            case RISCV_TRAP_IACCESS:
            case RISCV_TRAP_LACCESS:
            case RISCV_TRAP_SACCESS:
#endif
            case RISCV_TRAP_IPAGE:
            case RISCV_TRAP_LPAGE:
            case RISCV_TRAP_SPAGE:
                // Memory access faults go to the SIGSEGV handler.
                sched_raise_from_isr(kctx->thread, true, proc_sigsegv_handler);
                kctx->thread->kernel_isr_ctx.regs.a0 = tval;
                isr_ctx_swap(kctx);
                return;

            case RISCV_TRAP_IILLEGAL:
                // Illegal instruction faults go to the SIGILL handler.
                sched_raise_from_isr(kctx->thread, true, proc_sigill_handler);
                isr_ctx_swap(kctx);
                return;

            case RISCV_TRAP_EBREAK:
                // Breakpoints go to the SIGTRAP handler.
                sched_raise_from_isr(kctx->thread, true, proc_sigtrap_handler);
                isr_ctx_swap(kctx);
                return;
        }
    }

    // Unhandled trap.
    rawprint("\033[0m");
    if (fault3) {
        rawprint("**** TRIPLE FAULT ****\n");
        panic_poweroff();
    } else if (fault2) {
        rawprint("**** DOUBLE FAULT ****\n");
    }

    // Print trap name.
    if (trapno < TRAPNAMES_LEN && trapnames[trapno]) {
        rawprint(trapnames[trapno]);
    } else {
        rawprint("Trap 0x");
        rawprinthex(cause, 8);
    }

    // Print PC.
    asm volatile("csrr %0, " CSR_EPC_STR : "=r"(epc));
    rawprint(" at PC 0x");
    rawprinthex(epc, sizeof(size_t) * 2);

    // Print trap value.
    if ((1 << trapno) & MEM_ADDR_TRAPS) {
        rawprint(" while accessing 0x");
        rawprinthex(tval, sizeof(size_t) * 2);
    } else if (tval && trapno == RISCV_TRAP_IILLEGAL) {
        rawprint(" while decoding 0x");
        rawprinthex(tval, 8);
    }

    rawputc('\n');

#if MEMMAP_VMEM
    // Print what page table thinks.
    if ((1 << trapno) & MEM_ADDR_TRAPS) {
        virt2phys_t info = memprotect_virt2phys(kctx->mpu_ctx, tval);
        if (info.flags & MEMPROTECT_FLAG_RWX) {
            rawprint("Memory at this address: ");
            rawputc(info.flags & MEMPROTECT_FLAG_R ? 'r' : '-');
            rawputc(info.flags & MEMPROTECT_FLAG_W ? 'w' : '-');
            rawputc(info.flags & MEMPROTECT_FLAG_X ? 'x' : '-');
            rawputc(info.flags & MEMPROTECT_FLAG_KERNEL ? 'k' : 'u');
            rawputc(info.flags & MEMPROTECT_FLAG_GLOBAL ? 'g' : '-');
            rawprint("\nPhysical address: 0x");
            rawprinthex(info.paddr, 2 * sizeof(size_t));
            rawputc('\n');
        } else {
            rawprint("No memory at this address.\n");
        }
    }
#endif

    // Print privilige mode.
    if (!fault2) {
        if (status & (CSR_STATUS_PP_MASK << CSR_STATUS_PP_BASE_BIT)) {
            rawprint("Running in kernel mode");
            if (!(kctx->flags & ISR_CTX_FLAG_KERNEL)) {
                rawprint(" (despite is_kernel_thread=0)");
            }
        } else {
            rawprint("Running in user mode");
            if (kctx->flags & ISR_CTX_FLAG_KERNEL) {
                rawprint(" (despite is_kernel_thread=1)");
            }
        }
    }

    // Print current process.
    if (!fault2 && kctx->thread && !(kctx->thread->flags & THREAD_KERNEL)) {
        rawprint(" in process ");
        rawprintdec(kctx->thread->process->pid, 1);
    }
    rawputc('\n');
    backtrace_from_ptr(kctx->frameptr);

    isr_ctx_dump(kctx);

    if ((status & (CSR_STATUS_PP_MASK << CSR_STATUS_PP_BASE_BIT)) || fault2) {
        // When the kernel traps it's a bad time.
        panic_poweroff();
    } else {
        // When the user traps just stop the process.
        sched_raise_from_isr(kctx->thread, false, kill_proc_on_trap);
    }
    isr_ctx_swap(kctx);
}

// Return a value from the syscall handler.
void syscall_return(long long value) {
    sched_thread_t *thread = isr_ctx_get()->thread;
    isr_ctx_t      *usr    = &thread->user_isr_ctx;
    usr->regs.a0           = value;
#if __riscv_xlen == 32
    usr->regs.a1 = value >> 32;
#endif
    usr->regs.pc += 4;
    if (proc_signals_pending_raw(thread->process)) {
        proc_signal_handler();
    }
    irq_disable();
    sched_lower_from_isr();
    isr_context_switch();
    assert_unreachable();
}
