
// SPDX-License-Identifier: MIT

#pragma once

#include "cpu/regs.h"

#ifndef __ASSEMBLER__
#include "cpulocal.h"
#include "log.h"
#include "memprotect.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef struct sched_thread_t sched_thread_t;
typedef struct isr_ctx_t      isr_ctx_t;
// Custom trap handler callback, returns true if the trap was suppressed.
typedef bool (*isr_noexc_cb_t)(isr_ctx_t *ctx, void *cookie);
#endif

#ifdef __ASSEMBLER__

#define STRUCT_BEGIN(structname)
#if __riscv_xlen == 64
#define STRUCT_FIELD_WORD(structname, name, offset)         .equ structname##_##name, offset * 8
#define STRUCT_FIELD_PTR(structname, type, name, offset)    .equ structname##_##name, offset * 8
#define STRUCT_FIELD_STRUCT(structname, type, name, offset) .equ structname##_##name, offset * 8
#else
#define STRUCT_FIELD_WORD(structname, name, offset)         .equ structname##_##name, offset * 4
#define STRUCT_FIELD_PTR(structname, type, name, offset)    .equ structname##_##name, offset * 4
#define STRUCT_FIELD_STRUCT(structname, type, name, offset) .equ structname##_##name, offset * 4
#endif
#define STRUCT_END(structname)

#else

#define STRUCT_BEGIN(structname)                                                                                       \
    typedef struct structname structname;                                                                              \
    struct structname {
#define STRUCT_FIELD_WORD(structname, name, offset)         size_t name;
#define STRUCT_FIELD_PTR(structname, type, name, offset)    type *name;
#define STRUCT_FIELD_STRUCT(structname, type, name, offset) type name;
#define STRUCT_END(structname)                                                                                         \
    }                                                                                                                  \
    ;

#endif



// Context for interrupts, exceptions and traps in relation to threads.
STRUCT_BEGIN(isr_ctx_t)
// Scratch words for use by the ASM code.
STRUCT_FIELD_WORD(isr_ctx_t, scratch0, 0)
STRUCT_FIELD_WORD(isr_ctx_t, scratch1, 1)
STRUCT_FIELD_WORD(isr_ctx_t, scratch2, 2)
STRUCT_FIELD_WORD(isr_ctx_t, scratch3, 3)
STRUCT_FIELD_WORD(isr_ctx_t, scratch4, 4)
STRUCT_FIELD_WORD(isr_ctx_t, scratch5, 5)
// Pointer to currently active memory protection information.
STRUCT_FIELD_PTR(isr_ctx_t, mpu_ctx_t, mpu_ctx, 6)
// Frame pointer to use for backtraces.
STRUCT_FIELD_PTR(isr_ctx_t, void, frameptr, 7)
// Registers storage.
// The trap/interrupt handler will save registers to here.
// *Note: The syscall handler only saves/restores t0-t3, sp, gp, tp and ra, any other registers are not visible to the
// kernel.*
STRUCT_FIELD_STRUCT(isr_ctx_t, cpu_regs_t, regs, 8)
// Pointer to next isr_ctx_t to switch to.
// If nonnull, the trap/interrupt handler will context switch to this new context before exiting.
STRUCT_FIELD_PTR(isr_ctx_t, isr_ctx_t, ctxswitch, 40)
// Pointer to owning sched_thread_t.
STRUCT_FIELD_PTR(isr_ctx_t, sched_thread_t, thread, 41)
// Kernel context flags, only 32 bits available even on 64-bit targets.
STRUCT_FIELD_WORD(isr_ctx_t, flags, 42)
// CPU ID because S-mode software can't read it after startup.
STRUCT_FIELD_WORD(isr_ctx_t, cpuid, 43)
// Custom trap handler to call.
STRUCT_FIELD_STRUCT(isr_ctx_t, isr_noexc_cb_t, noexc_cb, 44)
// Cookie for custom trap handler.
STRUCT_FIELD_PTR(isr_ctx_t, void, noexc_cookie, 44)
// Pointer to CPU-local struct.
STRUCT_FIELD_PTR(isr_ctx_t, cpulocal_t, cpulocal, 45)
STRUCT_END(isr_ctx_t)

// `isr_ctx_t` flag: Is a kernel thread.
#define ISR_CTX_FLAG_KERNEL    0x00000001
// `isr_ctx_t` flag: For not set `sp` to the ISR stack.
#define ISR_CTX_FLAG_USE_SP    0x00000002
// `isr_ctx_t` flag: Currently running custom trap-handling code.
#define ISR_CTX_FLAG_NOEXC     0x00000004
// `isr_ctx_t` flag: SUM was set.
#define ISR_CTX_FLAG_RISCV_SUM (1 << RISCV_STATUS_SUM_BIT)
// `isr_ctx_t` flag: MXR was set.
#define ISR_CTX_FLAG_RISCV_MXR (1 << RISCV_STATUS_MXR_BIT)


#ifndef __ASSEMBLER__

// Stack alignment is defined to be 16 by the RISC-V calling convention
enum {
    STACK_ALIGNMENT = 16,
};

// Get the current ISR context.
static inline isr_ctx_t *isr_ctx_get() {
    isr_ctx_t *kctx;
    asm("csrr %0, " CSR_SCRATCH_STR : "=r"(kctx));
    return kctx;
}
// Get the outstanding context swap target, if any.
static inline isr_ctx_t *isr_ctx_switch_get() {
    isr_ctx_t *kctx;
    asm("csrr %0, " CSR_SCRATCH_STR : "=r"(kctx));
    return kctx->ctxswitch;
}
// Set the context swap target to swap to before exiting the trap/interrupt handler.
static inline void isr_ctx_switch_set(isr_ctx_t *switch_to) {
    isr_ctx_t *kctx;
    asm("csrr %0, " CSR_SCRATCH_STR : "=r"(kctx));
    kctx->ctxswitch = switch_to;
}
// Immediately swap the ISR context handle.
static inline isr_ctx_t *isr_ctx_swap(isr_ctx_t *kctx) {
    isr_ctx_t *old;
    asm("csrrw %0, " CSR_SCRATCH_STR ", %1" : "=r"(old) : "r"(kctx));
    kctx->ctxswitch = old->ctxswitch;
    return old;
}
// Print a register dump given isr_ctx_t.
void isr_ctx_dump(isr_ctx_t const *ctx);
// Print a register dump of the current registers.
void kernel_cur_regs_dump();
#endif



#undef STRUCT_BEGIN
#undef STRUCT_FIELD_WORD
#undef STRUCT_FIELD_PTR
#undef STRUCT_END
