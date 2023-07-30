#ifndef BADGEROS_KERNEL
#include <stdio.h>
#include <stdlib.h>
#endif

#include "allocator.h"

#include <stdatomic.h>
#include <stdint.h>

#define BITMAP_WORDS        4
#define DATA_OFFSET         64
#define PAGE_ALLOC_BITS(x)  (((PAGE_SIZE - 32) - ((x)-1)) / (x))
#define PAGE_ALLOC_BYTES(x) ((PAGE_ALLOC_BITS(x) + 7) / 8)

#define SKIP_LIST_MAX_LEVEL 5
#define SPIN_WAIT_COUNT     25

#define DEALLOC_VALUE 0xDEAD         // way larger than the largest amount of entries
#define DEALLOC_MIN   0x0BAD         // too far removed fro DEALLOC_VALUE to ever be reached in a race
#define LOCK_VALUE    (void *)0xCAFE // not page aligned can not happen

static uint16_t slab_bytes[]    = {32, 64, 128, 256};
static uint16_t slab_entries[]  = {126, 63, 31, 15};

// This needs to be correct otherwise finding an empty slab slot will not work
static uint32_t slab_empty[][4] = {
    {UINT32_MAX, UINT32_MAX, UINT32_MAX, 0x3FFFFFFF},
    {UINT32_MAX, 0x7FFFFFFF, 0, 0},
    {0x7FFFFFFF, 0, 0, 0},
    {0x00007FFF, 0, 0, 0}};

typedef struct slab_header {
    atomic_uchar          size;
    atomic_uchar          status;
    atomic_ushort         use_count;
    atomic_uintptr_t      next[SKIP_LIST_MAX_LEVEL];
    atomic_uint_least32_t bitmap[4];
} slab_header_t;

enum slab_status_t {
    SLAB_STATUS_ACTIVE,
    SLAB_STATUS_INACTIVE,
    SLAB_STATUS_ACTIVE_FULL,
    SLAB_STATUS_DEALLOCATED,
};

typedef struct skiplist {
    slab_header_t      head;
    enum slab_status_t in;
    enum slab_status_t out;
    atomic_flag        write_lock;
} skiplist_t;

static skiplist_t slab_head_active[4] = {
    {.head = {0}, .in = SLAB_STATUS_ACTIVE, .out = SLAB_STATUS_ACTIVE_FULL, .write_lock = ATOMIC_FLAG_INIT},
    {.head = {0}, .in = SLAB_STATUS_ACTIVE, .out = SLAB_STATUS_ACTIVE_FULL, .write_lock = ATOMIC_FLAG_INIT},
    {.head = {0}, .in = SLAB_STATUS_ACTIVE, .out = SLAB_STATUS_ACTIVE_FULL, .write_lock = ATOMIC_FLAG_INIT},
    {.head = {0}, .in = SLAB_STATUS_ACTIVE, .out = SLAB_STATUS_ACTIVE_FULL, .write_lock = ATOMIC_FLAG_INIT}};

static skiplist_t slab_head_inactive[4] = {
    {.head = {0}, .in = SLAB_STATUS_INACTIVE, .out = SLAB_STATUS_DEALLOCATED, .write_lock = ATOMIC_FLAG_INIT},
    {.head = {0}, .in = SLAB_STATUS_INACTIVE, .out = SLAB_STATUS_DEALLOCATED, .write_lock = ATOMIC_FLAG_INIT},
    {.head = {0}, .in = SLAB_STATUS_INACTIVE, .out = SLAB_STATUS_DEALLOCATED, .write_lock = ATOMIC_FLAG_INIT},
    {.head = {0}, .in = SLAB_STATUS_INACTIVE, .out = SLAB_STATUS_DEALLOCATED, .write_lock = ATOMIC_FLAG_INIT}};

static atomic_uint_least32_t inactive_count[4] = {0};

// static atomic_uintptr_t         slab_cache[] = {0, 0, 0, 0};
static atomic_flag slab_alloc_lock[]   = {ATOMIC_FLAG_INIT, ATOMIC_FLAG_INIT, ATOMIC_FLAG_INIT, ATOMIC_FLAG_INIT};

#ifndef BADGEROS_KERNEL
#include <assert.h>

static_assert((sizeof(slab_header_t) <= DATA_OFFSET), "Slab header must be smaller than DATA_OFFSET");
#endif

/* comment so clang-format is happy */

static inline bool bitmap_get_bit(const uint32_t word, uint8_t bit_index) {
    return (word & ((uint32_t)1 << bit_index)) != 0;
}

static inline uint32_t bitmap_set_bit(const uint32_t word, uint8_t bit_index) {
    return word | ((uint32_t)1 << bit_index);
}

static inline uint32_t bitmap_clear_bit(const uint32_t word, uint8_t bit_index) {
    return word & ~((uint32_t)1 << bit_index);
}

static void init_slab(slab_header_t *header, const enum slab_sizes size) {
    for (uint32_t i = 0; i < BITMAP_WORDS; ++i) {
        atomic_store_explicit(&header->bitmap[i], slab_empty[size][i], memory_order_release);
    }

    for (uint32_t i = 0; i < SKIP_LIST_MAX_LEVEL; ++i) {
        atomic_store_explicit(&header->next[i], 0, memory_order_release);
    }

    atomic_store_explicit(&header->size, size, memory_order_release);
    atomic_store_explicit(&header->use_count, 1, memory_order_release);
    atomic_store_explicit(&header->status, SLAB_STATUS_ACTIVE_FULL, memory_order_release);
}

static inline uint8_t determine_node_height(void *ptr) {
    uint8_t level = (uint8_t)((((uintptr_t)ptr >> 12) & 0x3) + 1);
    return level;
}

static void insert_slab_sorted(skiplist_t *list, slab_header_t *header, bool nonblock) {
    do {
        if (SPIN_LOCK_TRY_LOCK(list->write_lock)) {
            break;
        }

        if (nonblock)
            return;
    } while (true);

    uint8_t expected = list->out;
    uint8_t desired  = list->in;
    if (!atomic_compare_exchange_strong_explicit(
            &header->status,
            &expected,
            desired,
            memory_order_acq_rel,
            memory_order_acquire
        )) {
        goto out;
    }

    // We are definitely the only writers, and we just issused a memory_order_acquire fence above.
    for (uint32_t i = 0; i < SKIP_LIST_MAX_LEVEL; ++i) {
        atomic_store_explicit(&header->next[i], 0, memory_order_release);
    }

    slab_header_t *update[SKIP_LIST_MAX_LEVEL];
    slab_header_t *current = &list->head;

    for (int i = SKIP_LIST_MAX_LEVEL - 1; i >= 0; i--) {
        while ((slab_header_t *)current->next[i] != NULL && (slab_header_t *)current->next[i] < header) {
            current = (slab_header_t *)current->next[i];
        }
        update[i] = current;
    }

    int level = determine_node_height(header);
    for (int i = 0; i < level; i++) {
        header->next[i] = update[i]->next[i];
        atomic_store_explicit(&update[i]->next[i], (uintptr_t)header, memory_order_release);
    }

out:
    SPIN_LOCK_UNLOCK(list->write_lock);
}

static bool remove_slab(skiplist_t *list, slab_header_t *header, bool deleting, bool nonblock) {
    do {
        if (SPIN_LOCK_TRY_LOCK(list->write_lock)) {
            break;
        }

        if (nonblock)
            return false;
    } while (true);

    uint8_t expected = list->in;
    uint8_t desired  = list->out;
    bool    success  = false;

    if (!deleting) {
        // If we aren't deleting the page we can just have someone else try later without spinning
        if (!atomic_compare_exchange_strong_explicit(
                &header->status,
                &expected,
                desired,
                memory_order_acq_rel,
                memory_order_acquire
            )) {
            goto out;
        }
    } else {
        // If we are actually deleting the page we should just spin here until the page is actually removed
        do {
            if (!atomic_compare_exchange_strong_explicit(
                    &header->status,
                    &expected,
                    desired,
                    memory_order_acq_rel,
                    memory_order_acquire
                )) {
                if (expected == desired) {
                    goto out;
                }
            } else {
                break;
            }
        } while (true);
    }

    // We are definitely the only writers, and we just issused a memory_order_acquire fence above.

    slab_header_t *update[SKIP_LIST_MAX_LEVEL];
    slab_header_t *current = &list->head;

    for (int i = SKIP_LIST_MAX_LEVEL - 1; i >= 0; i--) {
        while ((slab_header_t *)current->next[i] != NULL && (slab_header_t *)current->next[i] < header) {
            current = (slab_header_t *)current->next[i];
        }
        update[i] = current;
    }

    if (current->next[0] && (slab_header_t *)current->next[0] == header) {
        slab_header_t *node_to_remove = (slab_header_t *)current->next[0];

        for (int i = 0; i < SKIP_LIST_MAX_LEVEL; i++) {
            if ((void *)update[i]->next[i] != node_to_remove)
                break;
            atomic_store_explicit(&update[i]->next[i], node_to_remove->next[i], memory_order_release);
        }
    }

    success = true;
out:
    SPIN_LOCK_UNLOCK(list->write_lock);
    return success;
}

static void *allocate_slab(const enum slab_sizes size) {
    if (!SPIN_LOCK_TRY_LOCK(slab_alloc_lock[size])) {
        return LOCK_VALUE;
    }

    slab_header_t *page = NULL;
    page                = (slab_header_t *)atomic_load(&slab_head_inactive[size].head.next[0]);
    if (page) {
        if (remove_slab(&slab_head_inactive[size], page, true, false)) {
            atomic_fetch_sub(&inactive_count[size], 1);
        } else {
            page = NULL;
        }
    }

    if (!page) {
        page = page_alloc(ALLOCATOR_SLAB, size);
    }

    if (!page) {

        SPIN_LOCK_UNLOCK(slab_alloc_lock[size]);
        // printf("Slab: allocating page for size %i failed\n", size);
        return NULL;
    }

    init_slab(page, size);
    insert_slab_sorted(&slab_head_active[size], page, false);

#ifndef BADGEROS_KERNEL
    // printf("allocate_slab(%i) page = %p\n", size, page);
#endif

    // atomic_store_explicit(&slab_cache[size], (uintptr_t)page, memory_order_release);
    SPIN_LOCK_UNLOCK(slab_alloc_lock[size]);
    return page;
}

static slab_header_t *find_last_node(skiplist_t *list) {
    if (!SPIN_LOCK_TRY_LOCK(list->write_lock)) {
        return NULL;
    }
    slab_header_t *current = &list->head;

    for (int i = SKIP_LIST_MAX_LEVEL - 1; i >= 0; i--) {
        for (int k = 0; k < SKIP_LIST_MAX_LEVEL; ++k) {
        }
        while ((void *)current->next[i] != NULL) {
            current = (slab_header_t *)current->next[i];
        }
    }
    SPIN_LOCK_UNLOCK(list->write_lock);
    return current;
}

static void deallocate_slab(void *page) {
    slab_header_t  *header = (slab_header_t *)page;
    enum slab_sizes size   = header->size;

#ifndef BADGEROS_KERNEL
    // printf("deallocate_slab(%p)\n", page);
    // if (tries > 1) printf("Deallocate_slab(%p) after %i tries\n", page, tries);
#endif

    SPIN_LOCK_LOCK(slab_alloc_lock[size]);
    remove_slab(&slab_head_active[size], page, true, false);
    atomic_store_explicit(&header->status, SLAB_STATUS_DEALLOCATED, memory_order_release);
    SPIN_LOCK_UNLOCK(slab_alloc_lock[size]);

    insert_slab_sorted(&slab_head_inactive[size], page, false);
    uint32_t inactive = atomic_fetch_add(&inactive_count[size], 1);

    while (inactive > 25) {
        void *inactive_page = find_last_node(&slab_head_inactive[size]);
        if (inactive_page && remove_slab(&slab_head_inactive[size], inactive_page, true, false)) {
            inactive = atomic_fetch_sub(&inactive_count[size], 1);
            page_free(inactive_page);
        } else {
            inactive = atomic_load(&inactive_count[size]);
        }
    }
}

void deallocate_inactive() {
    slab_header_t *page      = NULL;

    for (int size = 0; size < 4; ++size) {
        do {
            page = (slab_header_t *)atomic_load(&slab_head_inactive[size].head.next[0]);

            if (page) {
                remove_slab(&slab_head_inactive[size], page, true, false);
                atomic_fetch_sub(&inactive_count[size], 1);
                page_free(page);
            } else {
                break;
            }

        } while (true);
    }
}

static inline bool try_get_slab_page(void *page, const enum slab_sizes size) {
    slab_header_t *header = (slab_header_t *)page;

    // Immediately take ownership of the page, we might be racing a free
    // or we might have gotten this page at the same time as another thread
    uint32_t use_count    = atomic_fetch_add(&header->use_count, 1);

    // We might have raced here, and the page table is not atomic
    if (header->size != size || use_count >= slab_entries[size]) {
#ifndef BADGEROS_KERNEL
        // printf("get_slab_page(%i) page %p is full\n", size, page);
#endif
        if (use_count < DEALLOC_MIN) {
            // Only touch the page again if it is not being deallocated
            atomic_fetch_sub(&header->use_count, 1);

            // Slab is full lets stop looking at it, but don't try very hard
            remove_slab(&slab_head_active[header->size], page, false, true);
        }
        return false;
    }

    return true;
}

static void *get_slab_page(const enum slab_sizes size, uint32_t tries) {
    slab_header_t *page = NULL;

start:
    page = (slab_header_t *)atomic_load(&slab_head_active[size].head.next[0]);

    if (!page || tries > 10) {
        page = allocate_slab(size);
        if (page == LOCK_VALUE) {
            tries = 0;
            goto start;
        }
        return page;
    }

    if (!try_get_slab_page(page, size)) {
        goto start;
    }

    return page;
}

void *slab_alloc(size_t size) {
    if (size > 256)
        return NULL;

    uint8_t slab_type = SLAB_SIZE_32;
    if (size > 128)
        slab_type = SLAB_SIZE_256;
    else if (size > 64)
        slab_type = SLAB_SIZE_128;
    else if (size > 32)
        slab_type = SLAB_SIZE_64;

    slab_header_t *page  = NULL;

    uint32_t       tries = 0;
start_alloc:
    page = get_slab_page(slab_type, tries);
    // printf("Got page: %p\n", page);
    if (!page) {
        // printf("slab_alloc: Could not find a page for size %i\n", slab_type);
        return NULL;
    }

    for (uint32_t i = 0; i < BITMAP_WORDS; ++i) {
        uint32_t word = atomic_load(&page->bitmap[i]);

        if (word == 0)
            continue;

        uint32_t bit_index = ffs32((int32_t)word) - 1;
        uint32_t desired   = bitmap_clear_bit(word, bit_index);

        if (!atomic_compare_exchange_strong(&page->bitmap[i], &word, desired)) {
            // printf("-");
            goto out;
        }

        size_t index  = (i * 32) + bit_index;
        void  *retval = ((uint8_t *)page) + DATA_OFFSET + (index * slab_bytes[slab_type]);
#ifndef BADGEROS_KERNEL
        // printf(
        //     "Allocating %p word_index %i bit_index %i use_count: %i size: %i pointer: (%p)\n",
        //     page,
        //     i,
        //     bit_index,
        //     atomic_load(&page->use_count),
        //     slab_bytes[slab_type],
        //     retval
        //);
#endif
        // printf("*");
        // if(tries > 10) printf("Allocation of %p succeeded after %i tries\n", retval, tries);
        return retval;
    }

out:
    atomic_fetch_sub(&page->use_count, 1);
    // printf(".");
    ++tries;
    goto start_alloc;
}

void slab_free(void *ptr) {
    if (!ptr) {
#ifndef BADGEROS_KERNEL
        printf("slab_free: Attempting to free NULL\n");
#endif
        return;
    }

    slab_header_t *page       = ALIGN_PAGE_DOWN(ptr);

    size_t         page_index = get_page_index(page);
    uint8_t        page_type  = get_page_type(page_index);

    if (page_type != ALLOCATOR_SLAB) {
#ifndef BADGEROS_KERNEL
        printf("slab_free: Attempting to free an allocation of wrong type\n");
#endif
        return;
    }

    slab_header_t  *header    = (slab_header_t *)page;
    enum slab_sizes size      = header->size;
    size_t          offset    = (size_t)(ptr) - (size_t)(page);
    offset                   -= DATA_OFFSET;
    uint32_t total_bit_index  = offset / slab_bytes[header->size];
    uint32_t word_index       = total_bit_index / 32;
    uint32_t bit_index        = total_bit_index % 32;

    uint32_t expected, desired;
    do {
        expected = atomic_load(&header->bitmap[word_index]);
        desired  = bitmap_set_bit(expected, bit_index);
        if (expected == desired) {
#ifndef BADGEROS_KERNEL
            // printf(
            //     "Duplicate free %p word_index %i bit_index %i size: %i pointer: %p\n",
            //     page,
            //     word_index,
            //     bit_index,
            //     slab_bytes[header->size],
            //     ptr
            //);
#endif
            return;
        } else {
        }
    } while (!atomic_compare_exchange_weak_explicit(
        &header->bitmap[word_index],
        &expected,
        desired,
        memory_order_acq_rel,
        memory_order_acquire
    ));

    atomic_fetch_sub(&header->use_count, 1);
    // uint32_t use_count = atomic_fetch_sub(&header->use_count, 1);
#ifndef BADGEROS_KERNEL
    // printf(
    //     "Free %p word_index %i bit_index %i use_count: %i size: %i pointer: (%p)\n",
    //     page,
    //     word_index,
    //     bit_index,
    //     use_count,
    //     slab_bytes[header->size],
    //     ptr
    //);
#endif
    uint16_t expected_use = 0;

    if (atomic_compare_exchange_strong_explicit(
            &header->use_count,
            &expected_use,
            DEALLOC_VALUE,
            memory_order_acq_rel,
            memory_order_acquire
        )) {
        // if (i)
        //     printf("Deallocating slab %p after %i tries\n", header, i);
        deallocate_slab(page);
        return;
    }

    // Slab has space again, add it back into the list, but don't try too hard
    // use expected here from the failed CAS loop as the current use count
    insert_slab_sorted(&slab_head_active[size], page, !(expected_use <= 2));
}
