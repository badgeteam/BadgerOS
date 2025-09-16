
// SPDX-FileCopyrightText: 2025 Julian Scheffers <julian@scheffers.net>
// SPDX-FileType: SOURCE
// SPDX-License-Identifier: MIT

#pragma once

#include "errno.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Note: These flags are copied from the Rust code, do not change them!

// Map memory as executable.
#define MM_FLAG_R 0b0000_0000_0010
// Map memory as writeable (reads must also be allowed).
#define MM_FLAG_W 0b0000_0000_0100
// Map memory as executable.
#define MM_FLAG_X 0b0000_0000_1000
// Map memory as user-accessible.
#define MM_FLAG_U 0b0000_0001_0000
// Map memory as global (exists in all page ASIDs).
#define MM_FLAG_G 0b0000_0010_0000
// Page was accessed since this flag was last cleared.
#define MM_FLAG_A 0b0000_0100_0000
// Page was written since this flag was last cleared.
#define MM_FLAG_D 0b0000_1000_0000

// Mark page as copy-on-write (W must be disabled).
#define MM_FLAG_COW 0b0001_0000_0000

// Map memory as I/O (uncached, no write coalescing).
#define MM_FLAG_IO 0b0100_0000_0000
// Map memory as uncached write coalescing.
#define MM_FLAG_NC 0b1000_0000_0000

// Map memory as read-write.
#define MM_FLAG_RW  (MM_FLAG_R | MM_FLAG_W)
// Map memory as read-execute.
#define MM_FLAG_RX  (MM_FLAG_R | MM_FLAG_X)
// Map memory as read-write-execute.
#define MM_FLAG_RWX (MM_FLAG_R | MM_FLAG_W | MM_FLAG_X)


typedef size_t vpn_t;
typedef size_t ppn_t;

// Memory management context.
typedef struct {
#if !CONFIG_NOMMU
    ppn_t pt_root_ppn;
#endif
} mem_ctx_t;



// Initialize the virtual memory subsystem.
void mem_vmm_init();

// Map a range of memory for the kernel at any virtual address.
// Returns the virtual page number where it was mapped.
errno_t mem_map_k(vpn_t *virt_base_out, vpn_t virt_len, ppn_t phys_base, uint32_t flags);
// Map a range of memory for a user page table at a specific virtual address.
errno_t mem_map_k_at(vpn_t virt_base, vpn_t virt_len, ppn_t phys_base, uint32_t flags);
/// Map a range of memory for a user page table at a specific virtual address.
errno_t mem_map_u_at(ppn_t pt_root_ppn, vpn_t virt_base, vpn_t virt_len, ppn_t phys_base, uint32_t flags);
