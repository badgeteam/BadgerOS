
// SPDX-License-Identifier: MIT

#include "memprotect.h"

#include "assertions.h"
#include "badge_strings.h"
#include "cpu/mmu.h"
#include "cpu/panic.h"
#include "isr_ctx.h"
#include "page_alloc.h"
#include "port/port.h"

// Page table walk result.
typedef struct {
    // Physical address of last loaded PTA.
    size_t    paddr;
    // Last loaded PTE.
    mmu_pte_t pte;
    // Page table level of PTE.
    uint8_t   level;
    // Whether the subject page was found.
    bool      found;
    // Whether the virtual address is valid.
    bool      vaddr_valid;
} pt_walk_t;



// HHDM length in pages.
size_t memprotect_hhdm_pages;
// Kernel virtual page number.
size_t memprotect_kernel_vpn;
// Kernel physical page number.
size_t memprotect_kernel_ppn;
// Kernel length in pages.
size_t memprotect_kernel_pages;

/* ==== Kernel sections ==== */
// NOLINTBEGIN
extern char const __start_text[];
extern char const __stop_text[];
extern char const __start_rodata[];
extern char const __stop_rodata[];
extern char const __start_data[];
extern char const __stop_data[];
// NOLINTEND

// For systems with VMEM: global MMU context.
mpu_ctx_t      mpu_global_ctx;
// All active non-global MMU contexts.
static dlist_t ctx_list = DLIST_EMPTY;



// Walk a page table; vaddr to paddr or return where the page table ends.
static pt_walk_t pt_walk(size_t pt_ppn, size_t vpn) {
    if (vpn >= mmu_half_pages && vpn < mmu_high_vpn) {
        return (pt_walk_t){0};
    }

    // Place the next VPN at the MSB for easy extraction.
    size_t    pte_addr = 0;
    mmu_pte_t pte      = {0};

    // Walk the page table.
    for (int i = 0; i < mmu_levels; i++) {
        int level  = mmu_levels - i - 1;
        pte_addr   = pt_ppn * MMU_PAGE_SIZE + mmu_vpn_part(vpn, level) * sizeof(mmu_pte_t);
        pte        = mmu_read_pte(pte_addr);
        size_t ppn = mmu_pte_get_ppn(pte);

        if (!mmu_pte_is_valid(pte)) {
            // PTE invalid.
            return (pt_walk_t){pte_addr, pte, level, false, true};
        } else if (mmu_pte_is_leaf(pte)) {
// Leaf PTE.
#if MMU_SUPPORT_SUPERPAGES
            if (level && ppn & ((1 << MMU_BITS_PER_LEVEL * level) - 1)) {
                // Misaligned superpage.
                logkf(LOG_FATAL, "PT corrupt: L%{d} leaf PTE @ %{size;x} (%{size;x}) misaligned", i, pte_addr, pte.val);
                logkf(LOG_FATAL, "Offending VADDR: %{size;x}", vpn * MMU_PAGE_SIZE);
                panic_abort();
            }
#else
            if (level) {
                // MMU does not support superpages.
                logkf(
                    LOG_FATAL,
                    "PT corrupt: L%{d} leaf PTE @ %{size;x} (%{size;x}) but MMU does not support superpages",
                    i,
                    pte_addr,
                    pte.val
                );
                logkf(LOG_FATAL, "Offending VADDR: %{size;x}", vpn * MMU_PAGE_SIZE);
                panic_abort();
            }
#endif
            // Valid leaf PTE.
            return (pt_walk_t){pte_addr, pte, level, true, true};
        } else if (level) {
            // Non-leaf PTE.
            pt_ppn = ppn;
        }
    }

    // Not a leaf node.
    logkf(LOG_FATAL, "PT corrupt: L0 PTE @ %{size;x} (%{size;x}) not a leaf PTE", pte_addr, pte.val);
    logkf(LOG_FATAL, "Offending VADDR: %{size;x}", vpn * MMU_PAGE_SIZE);
    panic_abort();
}

// Split a PTE into the next page table level to allow for edits of superpages.
// Returns PPN of new table.
static inline size_t pt_split(size_t pt_ppn, int pt_level, size_t pte_paddr, mmu_pte_t pte, size_t vpn) {
#if MMU_SUPPORT_SUPERPAGES
    (void)pt_ppn;
    (void)pt_level;
    (void)vpn;

    // Allocate new table.
    size_t next_pt = phys_page_alloc(1, false);
    assert_always(next_pt);

    // Populate with new entries.
    size_t   ppn   = mmu_pte_get_ppn(pte);
    uint32_t flags = mmu_pte_get_flags(pte);
    for (size_t i = 0; i < (1 << MMU_BITS_PER_LEVEL); i++) {
        size_t low_pte_paddr = next_pt * MMU_PAGE_SIZE + i * sizeof(mmu_pte_t);
        mmu_write_pte(low_pte_paddr, mmu_pte_new_leaf(ppn + i, flags));
    }

    // Overwrite old entry.
    mmu_write_pte(pte_paddr, mmu_pte_new(next_pt));
    return next_pt;
#else
    (void)pt_ppn;

    // MMU does not support superpages.
    logkf(
        LOG_FATAL,
        "PT corrupt: L%{d} leaf PTE @ %{size;x} (%{size;x}) but MMU does not support superpages",
        pt_level,
        pte_paddr,
        pte.val
    );
    logkf(LOG_FATAL, "Offending VADDR: %{size;x}", vpn * MMU_PAGE_SIZE);
    panic_abort();
#endif
}

// Walk from an arbitrary page table level to another to create a new mapping.
// Returns true if the top level of the page table was edited.
static bool pt_map_1(size_t pt_ppn, int pt_level, size_t vpn, size_t ppn, int pte_level, uint32_t flags) {
    bool top_edit      = pt_level == pte_level;
    int  orig_pt_level = pt_level;

    // Validate paramters.
    assert_dev_drop(pt_level >= 0 && pt_level < mmu_levels);
#if MMU_SUPPORT_SUPERPAGES
    assert_dev_drop(pte_level >= 0 && pte_level < mmu_levels);
#else
    assert_dev_drop(pte_level == 0);
    pte_level = 0;
#endif
    assert_dev_drop(flags & MEMPROTECT_FLAG_RWX);
    assert_dev_drop((flags & MEMPROTECT_FLAG_RW) != MEMPROTECT_FLAG_W);
    assert_dev_drop(pte_level <= pt_level);

    // Walk the page table, adding new tables as needed.
    for (; pte_level < pt_level; pt_level--) {
        size_t    pte_paddr = pt_ppn * MMU_PAGE_SIZE + mmu_vpn_part(vpn, pt_level) * sizeof(mmu_pte_t);
        // Too high; read PTE to go to next table.
        mmu_pte_t pte       = mmu_read_pte(pte_paddr);
        if (!mmu_pte_is_valid(pte)) {
            // Make new level of page table.
            size_t next_pt = phys_page_alloc(1, false);
            assert_always(next_pt);
            mmu_write_pte(pte_paddr, mmu_pte_new(next_pt));
            top_edit |= orig_pt_level == pt_level;
            pt_ppn    = next_pt;
        } else if (mmu_pte_is_leaf(pte)) {
            // Split page.
            pt_ppn    = pt_split(pt_ppn, pt_level, pte_paddr, pte, vpn);
            top_edit |= orig_pt_level == pt_level;
        } else {
            // Walk to next level of page table.
            pt_ppn = mmu_pte_get_ppn(pte);
        }
    }

    // At the correct level; write leaf PTE.
    size_t pte_paddr = pt_ppn * MMU_PAGE_SIZE + mmu_vpn_part(vpn, pte_level) * sizeof(mmu_pte_t);
    mmu_write_pte(pte_paddr, mmu_pte_new_leaf(ppn, flags));
    return top_edit;
}

// Walk from an arbitrary page table level to another to remove a mapping.
// Returns true if the top level of the page table was edited.
static bool pt_unmap_1(size_t pt_ppn, int pt_level, size_t vpn, int pte_level) {
    bool top_edit      = pt_level == pte_level;
    int  orig_pt_level = pt_level;

    // Validate paramters.
    assert_dev_drop(pt_level >= 0 && pt_level < mmu_levels);
#if MMU_SUPPORT_SUPERPAGES
    assert_dev_drop(pte_level >= 0 && pte_level < mmu_levels);
#else
    assert_dev_drop(pte_level == 0);
    pte_level = 0;
#endif
    assert_dev_drop(pte_level <= pt_level);

    // Walk the page table, adding new tables as needed.
    for (; pte_level < pt_level; pt_level--) {
        size_t    pte_paddr = pt_ppn * MMU_PAGE_SIZE + mmu_vpn_part(vpn, pte_level) * sizeof(mmu_pte_t);
        // Too high; read PTE to go to next table.
        mmu_pte_t pte       = mmu_read_pte(pte_paddr);
        if (!mmu_pte_is_valid(pte)) {
            // Nothing mapped here.
            return top_edit;
        } else if (mmu_pte_is_leaf(pte)) {
            // Split page.
            pt_ppn    = pt_split(pt_ppn, pt_level, pte_paddr, pte, vpn);
            top_edit |= orig_pt_level == pt_level;
        } else {
            // Walk to next level of page table.
            pt_ppn = mmu_pte_get_ppn(pte);
        }
    }

    // At the correct level; write leaf PTE.
    size_t pte_paddr = pt_ppn * MMU_PAGE_SIZE + mmu_vpn_part(vpn, pte_level) * sizeof(mmu_pte_t);
    mmu_write_pte(pte_paddr, MMU_PTE_NULL);
    return top_edit;
}

// Calculate the biggest superpage level that will fit a range.
static int pt_calc_superpage(int max_level, size_t vpn, size_t ppn, size_t max_len) {
#if MMU_SUPPORT_SUPERPAGES
    for (int i = max_level; i > 0; i--) {
        size_t super_len = 1LLU << (MMU_BITS_PER_LEVEL * i);
        if ((vpn & (super_len - 1)) == 0 && (ppn & (super_len - 1)) == 0 && max_len >= super_len) {
            return i;
        }
    }
#endif
    return 0;
}

// Walk from an arbitrary page table level to create new mappings.
// Returns true if the top level of the page table was edited.
static bool pt_map(size_t pt_ppn, int pt_level, size_t vpn, size_t ppn, size_t pages, uint32_t flags) {
    bool top_edit = false;
    while (pages) {
        int pte_level     = pt_calc_superpage(pt_level, vpn, ppn, pages);
        top_edit         |= pt_map_1(pt_ppn, pt_level, vpn, ppn, pte_level, flags);
        size_t super_len  = 1LLU << (MMU_BITS_PER_LEVEL * pte_level);
        vpn              += super_len;
        ppn              += super_len;
        pages            -= super_len;
    }
    return top_edit;
}

// Walk from an arbitrary page table level to remove mappings.
// Returns true if the top level of the page table was edited.
static bool pt_unmap(size_t pt_ppn, int pt_level, size_t vpn, size_t pages) {
    bool top_edit = false;
    while (pages) {
        int pte_level     = pt_calc_superpage(pt_level, vpn, 0, pages);
        top_edit         |= pt_unmap_1(pt_ppn, pt_level, vpn, pte_level);
        size_t super_len  = (1LLU << MMU_BITS_PER_LEVEL) << pte_level;
        vpn              += super_len;
        pages            -= super_len;
    }
    return top_edit;
}

// Broadcast global mappings.
static void broadcast_to(size_t dest_pt_ppn, size_t src_pt_ppn) {
    for (size_t i = 1 << (MMU_BITS_PER_LEVEL - 1); i < (1 << MMU_BITS_PER_LEVEL); i++) {
        mmu_pte_t pte = mmu_read_pte(src_pt_ppn * MMU_PAGE_SIZE + i * sizeof(mmu_pte_t));
        if (mmu_pte_is_valid(pte)) {
            mmu_write_pte(dest_pt_ppn * MMU_PAGE_SIZE + i * sizeof(mmu_pte_t), pte);
        }
    }
}



// Lookup virtual address to physical address.
virt2phys_t memprotect_virt2phys(mpu_ctx_t *ctx, size_t vaddr) {
    if (!ctx) {
        ctx = &mpu_global_ctx;
    }
    if (vaddr >= mmu_half_size && vaddr < mmu_high_vaddr) {
        return (virt2phys_t){0};
    }
    pt_walk_t walk = pt_walk(ctx->root_ppn, vaddr / MMU_PAGE_SIZE);
    if (walk.found) {
        size_t page_size  = MMU_PAGE_SIZE << (MMU_BITS_PER_LEVEL * walk.level);
        size_t page_paddr = mmu_pte_get_ppn(walk.pte) * MMU_PAGE_SIZE;
        return (virt2phys_t){
            mmu_pte_get_flags(walk.pte),
            page_paddr + (vaddr & (page_size - 1)),
            vaddr & ~(page_size - 1),
            page_paddr,
            page_size,
        };
    }
    return (virt2phys_t){0};
}



// Initialise memory protection driver.
void memprotect_init() {
    // Initialize MMU driver.
    mmu_init();

    // Allocate global page table.
    mpu_global_ctx.root_ppn = phys_page_alloc(1, false);
    assert_always(mpu_global_ctx.root_ppn);

    // HHDM mapping (without execute this time).
    pt_map(
        mpu_global_ctx.root_ppn,
        mmu_levels - 1,
        mmu_hhdm_vaddr / MMU_PAGE_SIZE,
        0,
        memprotect_hhdm_pages,
        MEMPROTECT_FLAG_RW | MEMPROTECT_FLAG_GLOBAL | MEMPROTECT_FLAG_KERNEL
    );
    // Create kernel mappings.
    size_t vpn = memprotect_kernel_vpn;
    size_t ppn = memprotect_kernel_ppn;
    size_t sect_len;
    // Kernel RX.
    sect_len = (__stop_text - __start_text) / MMU_PAGE_SIZE;
    assert_dev_drop((__stop_text - __start_text) % MMU_PAGE_SIZE == 0);
    pt_map(
        mpu_global_ctx.root_ppn,
        mmu_levels - 1,
        vpn,
        ppn,
        sect_len,
        MEMPROTECT_FLAG_RX | MEMPROTECT_FLAG_GLOBAL | MEMPROTECT_FLAG_KERNEL
    );
    vpn      += sect_len;
    ppn      += sect_len;
    // Kernel R.
    sect_len  = (__stop_rodata - __start_rodata) / MMU_PAGE_SIZE;
    assert_dev_drop((__stop_rodata - __start_rodata) % MMU_PAGE_SIZE == 0);
    pt_map(
        mpu_global_ctx.root_ppn,
        mmu_levels - 1,
        vpn,
        ppn,
        sect_len,
        MEMPROTECT_FLAG_R | MEMPROTECT_FLAG_GLOBAL | MEMPROTECT_FLAG_KERNEL
    );
    vpn      += sect_len;
    ppn      += sect_len;
    // Kernel RW.
    sect_len  = (__stop_data - __start_data) / MMU_PAGE_SIZE;
    assert_dev_drop((__stop_data - __start_data) % MMU_PAGE_SIZE == 0);
    pt_map(
        mpu_global_ctx.root_ppn,
        mmu_levels - 1,
        vpn,
        ppn,
        sect_len,
        MEMPROTECT_FLAG_RW | MEMPROTECT_FLAG_GLOBAL | MEMPROTECT_FLAG_KERNEL
    );

    // Switch over to new page table.
    atomic_thread_fence(memory_order_release);
    memprotect_swap_from_isr();
    logkf_from_isr(LOG_INFO, "Virtual memory initialized, %{d} paging levels", mmu_levels);
}

// Create a memory protection context.
void memprotect_create(mpu_ctx_t *ctx) {
    ctx->node     = DLIST_NODE_EMPTY;
    ctx->root_ppn = phys_page_alloc(1, false);
    assert_always(ctx->root_ppn);
    size_t src_vaddr  = mmu_hhdm_vaddr + mpu_global_ctx.root_ppn * MMU_PAGE_SIZE;
    size_t dest_vaddr = mmu_hhdm_vaddr + ctx->root_ppn * MMU_PAGE_SIZE;
    mem_copy((void *)dest_vaddr, (void const *)src_vaddr, MMU_PAGE_SIZE);
    dlist_append(&ctx_list, &ctx->node);
}

// Clean up a memory protection context.
void memprotect_destroy(mpu_ctx_t *ctx) {
    dlist_remove(&ctx_list, &ctx->node);
    logkf(LOG_DEBUG, "TODO: Memprotect cleanup");
}

// Add a memory protection region.
void memprotect_impl(mpu_ctx_t *ctx, size_t vpn, size_t ppn, size_t pages, uint32_t flags) {
    bool top_mod = false;

    // Add R if W is set.
    if (flags & MEMPROTECT_FLAG_W) {
        flags |= MEMPROTECT_FLAG_R;
    }

    // Apply change.
    if (flags & MEMPROTECT_FLAG_RWX) {
        top_mod = pt_map(ctx->root_ppn, mmu_levels - 1, vpn, ppn, pages, flags);
    } else {
        top_mod = pt_unmap(ctx->root_ppn, mmu_levels - 1, vpn, pages);
    }

    // Broadcast global changes.
    if (top_mod && (flags & MEMPROTECT_FLAG_GLOBAL)) {
        dlist_node_t *node = ctx_list.head;
        while (node) {
            broadcast_to(mpu_global_ctx.root_ppn, ((mpu_ctx_t *)node)->root_ppn);
            node = node->next;
        }
    }
}

// Add a memory protection region for user memory.
bool memprotect_u(proc_memmap_t *new_mm, mpu_ctx_t *ctx, size_t vaddr, size_t paddr, size_t length, uint32_t flags) {
    (void)new_mm;
    if ((vaddr | paddr | length) % MMU_PAGE_SIZE) {
        // Misaligned.
        return false;
    }
    vaddr  /= MMU_PAGE_SIZE;
    paddr  /= MMU_PAGE_SIZE;
    length /= MMU_PAGE_SIZE;
    if (vaddr + length > mmu_half_pages) {
        // Out of bounds.
        return false;
    }
    flags &= ~(MEMPROTECT_FLAG_GLOBAL | MEMPROTECT_FLAG_KERNEL);
    memprotect_impl(ctx, vaddr, paddr, length, flags);
    return true;
}

// Add a memory protection region for kernel memory.
bool memprotect_k(size_t vaddr, size_t paddr, size_t length, uint32_t flags) {
    if ((vaddr | paddr | length) % MMU_PAGE_SIZE) {
        // Misaligned.
        return false;
    }
    vaddr  /= MMU_PAGE_SIZE;
    paddr  /= MMU_PAGE_SIZE;
    length /= MMU_PAGE_SIZE;
    if (vaddr + length > mmu_half_pages) {
        // Out of bounds.
        return false;
    }
    flags |= MEMPROTECT_FLAG_GLOBAL | MEMPROTECT_FLAG_KERNEL;
    memprotect_impl(&mpu_global_ctx, vaddr, paddr, length, flags);
    return true;
}

// Commit pending memory protections, if any.
void memprotect_commit(mpu_ctx_t *ctx) {
    (void)ctx;
    // TODO: Run mmu_vmem_fence on other CPUs.
    mmu_vmem_fence();
    // TODO: Garbage collection is now safe to do.
}
