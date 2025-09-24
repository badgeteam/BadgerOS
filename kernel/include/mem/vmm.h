
// SPDX-FileCopyrightText: 2025 Julian Scheffers <julian@scheffers.net>
// SPDX-FileType: SOURCE
// SPDX-License-Identifier: MIT

#pragma once

#include "errno.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if CONFIG_NOMMU
#error "mem/vmm.h" included in a NOMMU kernel
#endif

// Note: These flags are copied from the Rust code, do not change them!

// Map memory as executable.
#define VMM_FLAG_R 0b000000000010
// Map memory as writeable (reads must also be allowed).
#define VMM_FLAG_W 0b000000000100
// Map memory as executable.
#define VMM_FLAG_X 0b000000001000
// Map memory as user-accessible.
#define VMM_FLAG_U 0b000000010000
// Map memory as global (exists in all page ASIDs).
#define VMM_FLAG_G 0b000000100000
// Page was accessed since this flag was last cleared.
#define VMM_FLAG_A 0b000001000000
// Page was written since this flag was last cleared.
#define VMM_FLAG_D 0b000010000000

// Mark page as copy-on-write (W must be disabled).
#define VMM_FLAG_COW 0b000100000000

// Map memory as I/O (uncached, no write coalescing).
#define VMM_FLAG_IO 0b010000000000
// Map memory as uncached write coalescing.
#define VMM_FLAG_NC 0b100000000000

// Map memory as read-write.
#define VMM_FLAG_RW  (VMM_FLAG_R | VMM_FLAG_W)
// Map memory as read-execute.
#define VMM_FLAG_RX  (VMM_FLAG_R | VMM_FLAG_X)
// Map memory as read-write-execute.
#define VMM_FLAG_RWX (VMM_FLAG_R | VMM_FLAG_W | VMM_FLAG_X)


// Note: These types are copied from the Rust code, do not change them!

typedef size_t vpn_t;
typedef size_t ppn_t;

// Memory management context.
typedef struct {
    ppn_t pt_root_ppn;
} vmm_ctx_t;

// Describes the result of a virtual to physical address translation.
typedef struct {
    // Virtual address of page start.
    vpn_t    page_vaddr;
    // Physical address of page start.
    ppn_t    page_paddr;
    // Size of the mapping in pages.
    size_t   size;
    // Physical address.
    size_t   paddr;
    // Flags of the mapping.
    uint32_t flags;
    // Whether the mapping exists; if false, only `vpn` and `size` are valid.
    bool     valid;
} virt2phys_t;



// Higher-half direct map virtual address.
extern size_t vmm_hhdm_size;
// Higher-half direct map address offset (paddr -> vaddr).
extern size_t vmm_hhdm_vaddr;
// Higher-half direct map size.
extern size_t vmm_hhdm_offset;
// Kernel base virtual address.
extern size_t vmm_kernel_vaddr;
// Kernel base physical address.
extern size_t vmm_kernel_paddr;



// Initialize the virtual memory subsystem.
void vmm_init();

// Create a new user page table.
errno_t vmm_create_user_ctx(vmm_ctx_t *pt_root_ppn_out);
// Destroy a user page table.
void    vmm_destroy_user_ctx(vmm_ctx_t pt_root_ppn);

// Map a range of memory for the kernel at any virtual address.
// Returns the virtual page number where it was mapped.
errno_t     vmm_map_k(vpn_t *virt_base_out, vpn_t virt_len, ppn_t phys_base, uint32_t flags);
// Map a range of memory for a user page table at a specific virtual address.
errno_t     vmm_map_k_at(vpn_t virt_base, vpn_t virt_len, ppn_t phys_base, uint32_t flags);
// Unmap a range of kernel memory.
errno_t     vmm_unmap_k(vpn_t virt_base, vpn_t virt_len);
// Map a range of memory for a user page table at a specific virtual address.
errno_t     vmm_map_u_at(vmm_ctx_t *ctx, vpn_t virt_base, vpn_t virt_len, ppn_t phys_base, uint32_t flags);
// Unmap a range of user memory.
errno_t     vmm_unmap_u(vmm_ctx_t *ctx, vpn_t virt_base, vpn_t virt_len);
// Translate a virtual to a physical address.
virt2phys_t vmm_virt2phys(vmm_ctx_t *ctx, size_t vaddr);
// Switch to a different user virtual memory context.
void        vmm_ctxswitch(vmm_ctx_t *ctx);
// Switch to the kernel virtual memory context.
void        vmm_ctxswitch_k();
