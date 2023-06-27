
// SPDX-License-Identifier: MIT

#pragma once

#ifndef __ASSEMBLER__
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#endif
#include <regs.h>

#ifdef __ASSEMBLER__

#define STRUCT_BEGIN(structname)
#define STRUCT_FIELD_WORD(structname, name, offset) \
	.equ structname ## _ ## name, offset
#define STRUCT_FIELD_PTR(structname, type, name, offset) \
	.equ structname ## _ ## name, offset
#define STRUCT_END(structname)

#else

#define STRUCT_BEGIN(structname) \
	typedef struct {
#define STRUCT_FIELD_WORD(structname, name, offset) \
	uint32_t name;
#define STRUCT_FIELD_PTR(structname, type, name, offset) \
	type *name;
#define STRUCT_END(structname) \
	} structname;

#endif



// Kernel thread context.
STRUCT_BEGIN(kernel_ctx_t)
// Scratch words for use by the ASM code.
STRUCT_FIELD_WORD(kernel_ctx_t, scratch0, 0)
STRUCT_FIELD_WORD(kernel_ctx_t, scratch1, 4)
STRUCT_FIELD_WORD(kernel_ctx_t, scratch2, 8)
STRUCT_FIELD_WORD(kernel_ctx_t, scratch3, 12)
STRUCT_FIELD_WORD(kernel_ctx_t, scratch4, 16)
STRUCT_FIELD_WORD(kernel_ctx_t, scratch5, 20)
STRUCT_FIELD_WORD(kernel_ctx_t, scratch6, 24)
STRUCT_FIELD_WORD(kernel_ctx_t, scratch7, 28)
// Pointer to registers storage.
STRUCT_FIELD_PTR(kernel_ctx_t, cpu_regs_t, regs, 32)
STRUCT_END(kernel_ctx_t)



#ifndef __ASSEMBLER__
// Get the current kernel context.
static inline kernel_ctx_t *kernel_ctx_get() {
	kernel_ctx_t *kctx;
	asm ("csrr %0, mscratch" : "=r" (kctx));
	return kctx;
}
// Set the current kernel context.
static inline void kernel_ctx_set(kernel_ctx_t *kctx) {
	// Disable interrupts.
	uint32_t mstatus;
	asm volatile ("csrrc %0, mstatus, %1" : "=r" (mstatus) : "r" (15));
	mstatus &= 15;
	// Write mscratch.
	asm volatile ("csrw mscratch, %0" :: "r" (kctx));
	// Re-enable interrupts if applicable.
	asm volatile ("csrs mstatus, %0" :: "r" (mstatus));
}
// Print a register dump given kernel_ctx_t.
void kernel_ctx_dump(const kernel_ctx_t *ctx);
// Print a register dump of the current registers.
void kernel_cur_regs_dump();
#endif



#undef STRUCT_BEGIN
#undef STRUCT_FIELD_WORD
#undef STRUCT_FIELD_PTR
#undef STRUCT_END
