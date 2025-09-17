
// SPDX-License-Identifier: MIT

#include "usercopy.h"

#include "assertions.h"
#include "badge_strings.h"
#include "interrupt.h"
#include "isr_ctx.h"
#include "mem/vmm.h"
#include "process/internal.h"
#if !CONFIG_NOMMU
#include "cpu/mmu.h"
#endif

// TODO: Convert to page fault intercepting memcpy.
// TODO: Migrate into generic process API.



// Determine string length in memory a user owns.
// Returns -1 if the user doesn't have access to any byte in the string.
ptrdiff_t strlen_from_user_raw(process_t *process, size_t user_vaddr, ptrdiff_t max_len) {
    if (!max_len) {
        return 0;
    }
    ptrdiff_t len = 0;

    // Check first page permissions.
    if (!(proc_map_contains_raw(process, user_vaddr, 1) & VMM_FLAG_R)) {
        return -1;
    }

    // String length loop.
    vmm_ctxswitch(&process->memmap.mem_ctx);
#if !RISCV_M_MODE_KERNEL
    asm("csrs sstatus, %0" ::"r"((1 << RISCV_STATUS_SUM_BIT) | (1 << RISCV_STATUS_MXR_BIT)));
#endif
    while (len < max_len && *(char const *)user_vaddr) {
        len++;
        user_vaddr++;
        if (user_vaddr % CONFIG_PAGE_SIZE == 0) {
            // Check further page permissions.
#if !RISCV_M_MODE_KERNEL
            asm("csrc sstatus, %0" ::"r"((1 << RISCV_STATUS_SUM_BIT) | (1 << RISCV_STATUS_MXR_BIT)));
#endif
            if (!(proc_map_contains_raw(process, user_vaddr, 1) & VMM_FLAG_R)) {
                return -1;
            }
#if !RISCV_M_MODE_KERNEL
            asm("csrs sstatus, %0" ::"r"((1 << RISCV_STATUS_SUM_BIT) | (1 << RISCV_STATUS_MXR_BIT)));
#endif
        }
    }
#if !RISCV_M_MODE_KERNEL
    asm("csrc sstatus, %0" ::"r"((1 << RISCV_STATUS_SUM_BIT) | (1 << RISCV_STATUS_MXR_BIT)));
#endif

    return len;
}

// Copy bytes from user to kernel.
// Returns whether the user has access to all of these bytes.
// If the user doesn't have access, no copy is performed.
bool copy_from_user_raw(process_t *process, void *kernel_vaddr, size_t user_vaddr, size_t len) {
    if (!proc_map_contains_raw(process, user_vaddr, len)) {
        return false;
    }
#if RISCV_M_MODE_KERNEL
    mem_copy(kernel_vaddr, (void const *)user_vaddr, len);
#else
    vmm_ctxswitch(&process->memmap.mem_ctx);
    asm("csrs sstatus, %0" ::"r"((1 << RISCV_STATUS_SUM_BIT) | (1 << RISCV_STATUS_MXR_BIT)));
    mem_copy(kernel_vaddr, (void *)user_vaddr, len);
    asm("csrc sstatus, %0" ::"r"((1 << RISCV_STATUS_SUM_BIT) | (1 << RISCV_STATUS_MXR_BIT)));
#endif
    return true;
}

// Copy from kernel to user.
// Returns whether the user has access to all of these bytes.
// If the user doesn't have access, no copy is performed.
bool copy_to_user_raw(process_t *process, size_t user_vaddr, void const *kernel_vaddr0, size_t len) {
    uint8_t const *kernel_vaddr = kernel_vaddr0;
    if (!proc_map_contains_raw(process, user_vaddr, len)) {
        return false;
    }
#if RISCV_M_MODE_KERNEL
    mem_copy((void *)user_vaddr, kernel_vaddr, len);
#else
    while (len) {
        virt2phys_t info = vmm_virt2phys(process->memmap.mem_ctx.pt_root_ppn, user_vaddr);
        assert_dev_drop(info.flags & VMM_FLAG_U);
        assert_dev_drop(~info.flags & VMM_FLAG_G);
        size_t max_len = info.size - (user_vaddr & (info.size - 1));
        max_len        = len < max_len ? len : max_len;
        mem_copy((uint8_t *)vmm_hhdm_vaddr + info.paddr, kernel_vaddr, max_len);
        len          -= max_len;
        user_vaddr   += max_len;
        kernel_vaddr += max_len;
    }
#endif
    return true;
}
