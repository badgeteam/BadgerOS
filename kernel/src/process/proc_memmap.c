
// SPDX-License-Identifier: MIT

#include "arrays.h"
#include "assertions.h"
#include "badge_strings.h"
#include "log.h"
#include "memprotect.h"
#include "page_alloc.h"
#include "panic.h"
#include "port/hardware_allocation.h"
#include "process/internal.h"
#include "process/process.h"
#include "process/types.h"
#include "scheduler/cpu.h"
#include "scheduler/types.h"

#if MEMMAP_VMEM
#include "cpu/mmu.h"
#ifdef PROC_MEMMAP_MAX_REGIONS
#error "When virtual memory is enabled, there must be no memmap region limit (PROC_MEMMAP_MAX_REGIONS)"
#endif
#endif

// Memory map address comparator.
static int proc_memmap_cmp(void const *a, void const *b) {
    proc_memmap_ent_t const *a_ptr = a;
    proc_memmap_ent_t const *b_ptr = b;
#if MEMMAP_VMEM
    if (a_ptr->vaddr < b_ptr->vaddr)
        return -1;
    if (a_ptr->vaddr > b_ptr->vaddr)
        return 1;
#else
    if (a_ptr->paddr < b_ptr->paddr)
        return -1;
    if (a_ptr->paddr > b_ptr->paddr)
        return 1;
#endif
    return 0;
}

// Memory map address search function.
static int proc_memmap_search(void const *a, void const *_b) {
    proc_memmap_ent_t const *a_ptr = a;
    size_t                   b     = (size_t)_b;
#if MEMMAP_VMEM
    if (a_ptr->vaddr < b)
        return -1;
    if (a_ptr->vaddr > b)
        return 1;
#else
    if (a_ptr->paddr < b)
        return -1;
    if (a_ptr->paddr > b)
        return 1;
#endif
    return 0;
}

#if MEMMAP_VMEM
// Allocate more memory to a process.
// Returns actual virtual address on success, 0 on failure.
size_t proc_map_raw(
    badge_err_t *ec, process_t *proc, size_t vaddr_req, size_t min_size, size_t min_align, uint32_t flags
) {
    proc_memmap_t *map = &proc->memmap;

    // Correct virtual address.
    if (min_align & (min_align - 1)) {
        logkf(LOG_WARN, "min_align=%{size;d} ignored because it is not a power of 2", min_align);
        min_align = MEMMAP_PAGE_SIZE;
    } else if (min_align < MEMMAP_PAGE_SIZE) {
        min_align = MEMMAP_PAGE_SIZE;
    }
    if (vaddr_req < 65536 || vaddr_req < MEMMAP_PAGE_SIZE) {
        vaddr_req = 65536 > MEMMAP_PAGE_SIZE ? 65536 : MEMMAP_PAGE_SIZE;
    }
    if (vaddr_req & (min_align - 1)) {
        vaddr_req += min_align - (vaddr_req & (min_align - 1));
    }
    if (min_size & (min_align - 1)) {
        min_size += min_align - (min_size & (min_align - 1));
    }
    if (vaddr_req + min_size > mmu_half_size) {
        vaddr_req = mmu_half_size - min_size;
        if (vaddr_req & (min_align - 1)) {
            vaddr_req -= vaddr_req & (min_align - 1);
        }
        if (vaddr_req < 65536 || vaddr_req < MEMMAP_PAGE_SIZE) {
            logk(LOG_WARN, "Impossible vmem request");
            badge_err_set(ec, ELOC_PROCESS, ECAUSE_NOMEM);
            return 0;
        }
    }

    // TODO: Disambiguate virtual addresses.
    uint32_t existing = proc_map_contains_raw(proc, vaddr_req, min_size);
    if (existing) {
        logk(LOG_WARN, "Overlapping virtual address requested");
        badge_err_set(ec, ELOC_PROCESS, ECAUSE_NOMEM);
        return 0;
    }

    // Convert to page numbers.
    size_t vpn   = vaddr_req / MEMMAP_PAGE_SIZE;
    size_t pages = min_size / MEMMAP_PAGE_SIZE;
    size_t i     = 0;
    while (i < pages) {
        size_t alloc, ppn;
        for (alloc = (size_t)1 << __builtin_ctzll(((pages - i) | (vpn + i)) / MEMMAP_PAGE_SIZE); alloc; alloc >>= 1) {
            ppn = phys_page_alloc(alloc, true);
            if (ppn) {
                break;
            }
        }
        if (!alloc) {
            badge_err_set(ec, ELOC_PROCESS, ECAUSE_NOMEM);
            goto error;
        }
        proc_memmap_ent_t new_ent = {
            .paddr = ppn * MEMMAP_PAGE_SIZE,
            .vaddr = (vpn + i) * MEMMAP_PAGE_SIZE,
            .size  = alloc * MEMMAP_PAGE_SIZE,
            .write = true,
            .exec  = true,
        };
        if (!array_lencap_sorted_insert(
                &map->regions,
                sizeof(proc_memmap_ent_t),
                &map->regions_len,
                &map->regions_cap,
                &new_ent,
                proc_memmap_cmp
            )) {
            badge_err_set(ec, ELOC_PROCESS, ECAUSE_NOMEM);
            phys_page_free(ppn);
            goto error;
        }
        if (!memprotect_u(
                &proc->memmap,
                &proc->memmap.mpu_ctx,
                (vpn + i) * MEMMAP_PAGE_SIZE,
                ppn * MEMMAP_PAGE_SIZE,
                alloc * MEMMAP_PAGE_SIZE,
                flags
            )) {
            badge_err_set(ec, ELOC_PROCESS, ECAUSE_NOMEM);
            goto error;
        }
        logkf(LOG_INFO, "Mapped %{size;d} bytes at %{size;x} to process %{d}", new_ent.size, new_ent.vaddr, proc->pid);
        i += alloc;
    }

    memprotect_commit(&proc->memmap.mpu_ctx);
    badge_err_set_ok(ec);
    return vaddr_req;
error:
    logk(LOG_WARN, "TODO: Cleanup when proc_map_raw partially fails");
    return 0;
}

// Split the memory map until `vaddr` to `vaddr+len` is an unique range.
static inline size_t split_regions(proc_memmap_t *map, size_t index, size_t vaddr, size_t len) {
    while (1) {
        proc_memmap_ent_t *ent = map->regions + index;
        if (ent->vaddr == vaddr && ent->size == len) {
            // Range matches memmap entry; we're done here.
            return index;
        }

        // Split entry.
        phys_page_split(ent->paddr / MEMMAP_PAGE_SIZE);
        ent->size                 /= 2;
        proc_memmap_ent_t new_ent  = *ent;
        new_ent.paddr             += new_ent.size;
        new_ent.vaddr             += new_ent.size;

        // Insert new entry in memory map.
        if (!array_lencap_insert(
                &map->regions,
                sizeof(proc_memmap_ent_t),
                &map->regions_len,
                &map->regions_cap,
                &new_ent,
                index + 1
            )) {
            logk(LOG_FATAL, "Out of memory");
            panic_abort();
        }

        if (new_ent.vaddr <= vaddr) {
            // Continue with second half instead of first half.
            index++;
        }
    }
}

// Release memory allocated to a process from `vaddr` to `vaddr+len`.
// The given span should not fall outside an area mapped with `proc_map_raw`.
void proc_unmap_raw(badge_err_t *ec, process_t *proc, size_t vaddr, size_t len) {
    proc_memmap_t *map = &proc->memmap;
    assert_dev_drop(proc_map_contains_raw(proc, vaddr, len));

    // Align `vaddr` and `len` to page sizes.
    if (vaddr % MEMMAP_PAGE_SIZE) {
        len   += vaddr % MEMMAP_PAGE_SIZE;
        vaddr -= vaddr % MEMMAP_PAGE_SIZE;
    }
    if (len % MEMMAP_PAGE_SIZE) {
        len += MEMMAP_PAGE_SIZE - len % MEMMAP_PAGE_SIZE;
    }

    // Array of pages to free.
    size_t *to_free     = NULL;
    size_t  to_free_len = 0, to_free_cap = 0;

    // Find start of mapped region.
    array_binsearch_t search =
        array_binsearch(map->regions, sizeof(proc_memmap_ent_t), map->regions_len, (void *)vaddr, proc_memmap_search);
    assert_dev_drop(search.found);
    size_t index = search.index;
    assert_dev_drop(map->regions[index].vaddr <= vaddr);
    assert_dev_drop(vaddr < map->regions[index].vaddr + map->regions[index].size);

    // Split mapped regions and unmap them from the process.
    while (len) {
        // Get sub-block offset; it dictates how to split and release memory.
        size_t page_off = vaddr - map->regions[index].vaddr;
        size_t order    = __builtin_ctzll((page_off | map->regions[index].size) / MEMMAP_PAGE_SIZE);

        // Split the memory and remove the region in question.
        size_t            split_index = split_regions(map, index, vaddr, MEMMAP_PAGE_SIZE << order);
        proc_memmap_ent_t removed;
        array_lencap_remove(
            &map->regions,
            sizeof(proc_memmap_ent_t),
            &map->regions_len,
            &map->regions_cap,
            &removed,
            split_index
        );

        // Add it to the list of blocks to free and remove it from the page table.
        size_t split_ppn = removed.paddr / MEMMAP_PAGE_SIZE;
        if (!array_lencap_insert(&to_free, sizeof(size_t), &to_free_len, &to_free_cap, &split_ppn, to_free_len)) {
            logk(LOG_FATAL, "Out of memory");
            panic_abort();
        }
        memprotect_u(map, &map->mpu_ctx, vaddr, 0, (size_t)MEMMAP_PAGE_SIZE << order, 0);
        logkf(
            LOG_INFO,
            "Unmapped %{size;d} bytes at %{size;x} from process %{d}",
            (size_t)MEMMAP_PAGE_SIZE << order,
            vaddr,
            proc->pid
        );

        // Remove it from the space to unmap.
        vaddr += (size_t)MEMMAP_PAGE_SIZE << order;
        len   -= (size_t)MEMMAP_PAGE_SIZE << order;
        index  = split_index;
    }

    // Commit the new page tables.
    memprotect_commit(&map->mpu_ctx);

    // Release those same pages.
    for (size_t i = 0; i < to_free_len; i++) {
        phys_page_free(to_free[i]);
    }
    free(to_free);
}

// Whether the process owns this range of virtual memory.
// Returns the lowest common denominator of the access bits.
int proc_map_contains_raw(process_t *proc, size_t vaddr, size_t size) {
    if (vaddr >= mmu_high_vaddr || vaddr + size > mmu_high_vaddr) {
        return 0;
    }
    int flags = MEMPROTECT_FLAG_RWX;
    while (true) {
        virt2phys_t info  = memprotect_virt2phys(&proc->memmap.mpu_ctx, vaddr);
        flags            &= (int)info.flags;
        if (!flags) {
            return 0;
        }
        size_t inc = info.page_size - (vaddr & (info.page_size - 1));
        if (inc >= size) {
            return flags;
        }
        size  -= inc;
        vaddr += inc;
    }
}

#else

// Allocate more memory to a process.
size_t proc_map_raw(
    badge_err_t *ec, process_t *proc, size_t vaddr_req, size_t min_size, size_t min_align, uint32_t flags
) {
    if (min_align & (min_align - 1)) {
        logkf(LOG_WARN, "min_align=%{size;d} ignored because it is not a power of 2", min_align);
    } else if (min_align > MEMMAP_PAGE_SIZE) {
        logkf(
            LOG_WARN,
            "min_align=%{size;d} not satisfiable because it is more than page size (%{size;d})",
            min_align,
            MEMMAP_PAGE_SIZE
        );
        return 0;
    }
    proc_memmap_t *map = &proc->memmap;

#ifdef PROC_MEMMAP_MAX_REGIONS
    if (map->regions_len >= PROC_MEMMAP_MAX_REGIONS) {
        logk(LOG_WARN, "Out of regions");
        badge_err_set(ec, ELOC_PROCESS, ECAUSE_NOMEM);
        return 0;
    }
#endif

    // Allocate memory to the process.
    min_size    = min_size ? (min_size - 1) / MEMMAP_PAGE_SIZE + 1 : 1;
    size_t base = phys_page_alloc(min_size, true) * MEMMAP_PAGE_SIZE;
    if (!base) {
        logk(LOG_WARN, "Out of memory");
        badge_err_set(ec, ELOC_PROCESS, ECAUSE_NOMEM);
        return 0;
    }
    size_t size = phys_page_size(base / MEMMAP_PAGE_SIZE) * MEMMAP_PAGE_SIZE;
    mem_set((void *)base, 0, size);
    vaddr_req = (size_t)base;

    // Account the process's memory.
    proc_memmap_ent_t new_ent = {
        .paddr = base,
        .size  = size,
        .write = true,
        .exec  = true,
    };
#ifdef PROC_MEMMAP_MAX_REGIONS
    array_sorted_insert(map->regions, sizeof(proc_memmap_ent_t), map->regions_len, &new_ent, proc_memmap_cmp);
    map->regions_len++;
#else
    array_lencap_sorted_insert(
        &map->regions,
        sizeof(proc_memmap_ent_t),
        &map->regions_len,
        &map->regions_cap,
        &new_ent,
        proc_memmap_cmp
    );
#endif

    // Update memory protection.
    if (!memprotect_u(map, &map->mpu_ctx, (size_t)base, vaddr_req, size, flags & MEMPROTECT_FLAG_RWX)) {
        for (size_t i = 0; i < map->regions_len; i++) {
            if (map->regions[i].paddr == (size_t)base) {
                array_remove(&map->regions[0], sizeof(map->regions[0]), map->regions_len, NULL, i);
                break;
            }
        }
        map->regions_len--;
        phys_page_free(base / MEMMAP_PAGE_SIZE);
        badge_err_set(ec, ELOC_PROCESS, ECAUSE_NOMEM);
        return 0;
    }
    memprotect_commit(&map->mpu_ctx);

    logkf(LOG_INFO, "Mapped %{size;d} bytes at %{size;x} to process %{d}", size, base, proc->pid);
    badge_err_set_ok(ec);
    return vaddr_req;
}

// Release memory allocated to a process.
void proc_unmap_raw(badge_err_t *ec, process_t *proc, size_t base) {
    proc_memmap_t *map = &proc->memmap;
    for (size_t i = 0; i < map->regions_len; i++) {
        if (map->regions[i].paddr == base) {
            proc_memmap_ent_t region = map->regions[i];
            array_remove(&map->regions[0], sizeof(map->regions[0]), map->regions_len, NULL, i);
            map->regions_len--;
            assert_dev_keep(memprotect_u(map, &map->mpu_ctx, base, base, region.size, 0));
            memprotect_commit(&map->mpu_ctx);
            phys_page_free(base / MEMMAP_PAGE_SIZE);
            badge_err_set_ok(ec);
            logkf(LOG_INFO, "Unmapped %{size;d} bytes at %{size;x} from process %{d}", region.size, base, proc->pid);
            return;
        }
    }
    badge_err_set(ec, ELOC_PROCESS, ECAUSE_NOTFOUND);
}

// Whether the process owns this range of virtual memory.
// Returns the lowest common denominator of the access bits.
int proc_map_contains_raw(process_t *proc, size_t vaddr, size_t size) {
    // Align to whole pages.
    if (vaddr % MEMMAP_PAGE_SIZE) {
        size  += vaddr % MEMMAP_PAGE_SIZE;
        vaddr -= vaddr % MEMMAP_PAGE_SIZE;
    }
    if (size % MEMMAP_PAGE_SIZE) {
        size += MEMMAP_PAGE_SIZE - size % MEMMAP_PAGE_SIZE;
    }

    int access = 7;
    while (size) {
        size_t i;
        for (i = 0; i < proc->memmap.regions_len; i++) {
            if (vaddr >= proc->memmap.regions[i].paddr &&
                vaddr < proc->memmap.regions[i].paddr + proc->memmap.regions[i].size) {
                goto found;
            }
        }

        // This page is not in the region map.
        return 0;

    found:
        // This page is in the region map.
        if (proc->memmap.regions[i].size > size) {
            // All pages found.
            break;
        }
        vaddr += proc->memmap.regions[i].size;
        size  += proc->memmap.regions[i].size;
    }
    return access;
}
#endif



// Allocate more memory to a process.
// Returns actual virtual address on success, 0 on failure.
size_t proc_map(badge_err_t *ec, pid_t pid, size_t vaddr_req, size_t min_size, size_t min_align, int flags) {
    mutex_acquire(&proc_mtx, TIMESTAMP_US_MAX);
    process_t *proc = proc_get(pid);
    size_t     res  = 0;
    if (proc) {
        res = proc_map_raw(ec, proc, vaddr_req, min_size, min_align, flags);
    } else {
        badge_err_set(ec, ELOC_PROCESS, ECAUSE_NOTFOUND);
    }
    mutex_release(&proc_mtx);
    return res;
}

// Release memory allocated to a process.
// The given span should not fall outside an area mapped with `proc_map_raw`.
void proc_unmap(badge_err_t *ec, pid_t pid, size_t vaddr, size_t len) {
    mutex_acquire(&proc_mtx, TIMESTAMP_US_MAX);
    process_t *proc = proc_get(pid);
    if (proc) {
        proc_unmap_raw(ec, proc, vaddr, len);
    } else {
        badge_err_set(ec, ELOC_PROCESS, ECAUSE_NOTFOUND);
    }
    mutex_release(&proc_mtx);
}

// Whether the process owns this range of memory.
// Returns the lowest common denominator of the access bits bitwise or 8.
int proc_map_contains(badge_err_t *ec, pid_t pid, size_t base, size_t size) {
    mutex_acquire_shared(&proc_mtx, TIMESTAMP_US_MAX);
    process_t *proc = proc_get(pid);
    int        ret  = 0;
    if (proc) {
        ret = proc_map_contains_raw(proc, base, size);
        badge_err_set_ok(ec);
    } else {
        badge_err_set(ec, ELOC_PROCESS, ECAUSE_NOTFOUND);
    }
    mutex_release_shared(&proc_mtx);
    return ret;
}
