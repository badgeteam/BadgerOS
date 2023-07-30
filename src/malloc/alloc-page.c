#include "allocator.h"

#include <stdatomic.h>
#include <stdbool.h>

#ifndef BADGEROS_KERNEL
#include <stdio.h>
#endif

static atomic_size_t       free_pages;
static atomic_flag         link_lock = ATOMIC_FLAG_INIT;
static bitmap_word_atomic *page_bitmap;

static size_t              pages;
static size_t              page_bitmap_size;
static uint8_t            *page_table;
static uint8_t            *mem_start;
static uint8_t            *mem_end;
static uint8_t            *pages_start;
static uint8_t            *pages_end;

/* A comment to unconfuse clang-format */

static inline bool page_bitmap_get_bit(const bitmap_word word, uint8_t bit_index) {
    return (word & ((bitmap_word)1 << bit_index)) != 0;
}

static inline bitmap_word page_bitmap_set_bit(const bitmap_word word, uint8_t bit_index) {
    return word | ((bitmap_word)1 << bit_index);
}

static inline bitmap_word page_bitmap_clear_bit(const bitmap_word word, uint8_t bit_index) {
    return word & ~((bitmap_word)1 << bit_index);
}

size_t get_free_pages() {
    return atomic_load(&free_pages);
}

size_t get_pages() {
    return pages;
}

bitmap_word *get_page_bitmap() {
    return (bitmap_word *)page_bitmap;
}

size_t get_page_bitmap_size() {
    return page_bitmap_size;
}

uint8_t is_page_free_idx(size_t page_index) {
    uint32_t    word_index = page_index / BITMAP_WORD_BITS;
    uint8_t     bit_index  = page_index % BITMAP_WORD_BITS;

    bitmap_word word       = atomic_load(&page_bitmap[word_index]);
    return page_bitmap_get_bit(word, bit_index);
}

uint8_t is_page_free(void *ptr) {
    size_t page_index = get_page_index(ptr);
    return is_page_free_idx(page_index);
}

static inline uint8_t type_data_pack(uint8_t type, uint8_t data) {
    type = type & 0x0F;
    data = data & 0x0F;

    return (type << 4) | data;
}

static inline uint8_t type_data_get_type(uint8_t packed) {
    return (packed >> 4) & 0x0F;
}

static inline uint8_t type_data_get_data(uint8_t packed) {
    return packed & 0x0F;
}

uint8_t get_page_type(size_t index) {
    return type_data_get_type(page_table[index]);
}

uint8_t get_page_data(size_t index) {
    return type_data_get_data(page_table[index]);
}

static inline uint8_t get_page_type_data(size_t index) {
    return page_table[index];
}

static inline void set_page_type_data(size_t index, enum allocator_type type, uint8_t data) {
    uint8_t type_data = type_data_pack(type, data);
    page_table[index] = type_data;
}

size_t get_page_index(void *ptr) {
    return ((size_t)ptr - (size_t)pages_start) / PAGE_SIZE;
}

size_t get_page_index_by_type_data(size_t start_index, enum allocator_type type, uint8_t data) {
    uint8_t type_data = type_data_pack(type, data);

    uint8_t t         = get_page_type_data(start_index);
    if (t == type_data) {
        return start_index;
    }

    start_index              += 1;
    uint32_t start            = (start_index / BITMAP_WORD_BITS);
    uint32_t start_bit_index  = (start_index % BITMAP_WORD_BITS);

    for (uint32_t i = start; i < (page_bitmap_size / BITMAP_WORD_BYTES); ++i) {
        bitmap_word word = atomic_load(&page_bitmap[i]);
        if (word == BITMAP_WORD_MAX)
            continue;

        for (uint8_t bit_index = start_bit_index; bit_index < BITMAP_WORD_BITS; ++bit_index) {
            if (page_bitmap_get_bit(word, bit_index))
                continue;

            size_t  index = (i * BITMAP_WORD_BITS) + bit_index;
            uint8_t t     = get_page_type_data(index);
            if (t == type_data) {
                return index;
            }
        }

        start_bit_index = 0;
    }

    return pages + 1;
}

void *get_page_by_index(size_t index) {
    return pages_start + (index * PAGE_SIZE);
}

static void page_bitmap_initialize() {
    for (size_t i = 0; i < page_bitmap_size / BITMAP_WORD_BYTES; i++) {
        atomic_store(&page_bitmap[i], BITMAP_WORD_MAX); // All pages are initially free
    }
}

static void page_table_initialize() {
    for (size_t i = 0; i < pages; i++) {
        page_table[i] = 0;
    }
}

void page_alloc_init(void *start, void *end) {
    mem_start              = (uint8_t *)start;
    mem_end                = (uint8_t *)end;

    uint8_t *first_page    = ALIGN_PAGE_UP(mem_start);
    pages_end              = ALIGN_PAGE_DOWN((mem_end));
    pages                  = (((size_t)pages_end) - ((size_t)first_page)) / PAGE_SIZE;

    size_t page_table_size = pages;
    page_bitmap_size       = ((((pages + 7) / 8) + BITMAP_WORD_BYTES - 1) / BITMAP_WORD_BYTES) * BITMAP_WORD_BYTES;

    page_table             = mem_start;
    page_bitmap            = (bitmap_word_atomic *)ALIGN_UP(mem_start + page_table_size, BITMAP_WORD_BYTES);
    pages_start            = ALIGN_PAGE_UP(((char *)page_bitmap) + page_bitmap_size);

    pages = (((size_t)pages_end) - ((size_t)pages_start)) / PAGE_SIZE; // Need to recaculate the number of pages now
    free_pages = pages;

    page_bitmap_initialize();
    page_table_initialize();

#ifndef BADGEROS_KERNEL
    size_t waste = (((size_t)pages_start - (size_t)mem_start)) - (page_table_size + page_bitmap_size);

    printf(
        "Memory starts at: %p, Memory ends at: %p, First page at: %p, "
        "total_pages: %zi, page_table_size: %zi, page_bitmap_size: %zi, waste: %zi, usable memory: %zi\n",
        start,
        end,
        pages_start,
        pages,
        page_table_size,
        page_bitmap_size,
        waste,
        pages * PAGE_SIZE
    );
    printf("Page bitmap at %p\n", page_bitmap);
#endif
}

static inline bool page_alloc_link_word(size_t word_index, uint8_t bit_index, uint8_t size) {
    if (!size)
        return false;

    if (bit_index + size > BITMAP_WORD_BITS)
        return false;

    bitmap_word expected, desired;
    if (bit_index == 0 && size == BITMAP_WORD_BITS) {
        expected = BITMAP_WORD_MAX;
        desired  = 0;

        if (!atomic_compare_exchange_strong(&page_bitmap[word_index], &expected, desired)) {
            // printf("page_alloc_link_word() Failed to alloc whole word\n");
            return true;
        }
    } else {
        do {
            expected = atomic_load(&page_bitmap[word_index]);
            desired  = expected;

            // Check each page as it currently is to make sure we didn't get raced
            for (uint8_t i = bit_index; i < bit_index + size; ++i) {
                if (expected & ((bitmap_word)1 << i)) {
                    desired &= ~((bitmap_word)1 << i);
                } else {
                    // We got raced here, abort
                    // printf("page_alloc_link_word() Failed to alloc word_index: %zi, bit_index: %i, size: %zi\n",
                    // word_index, bit_index, size);
                    return true;
                }
            }
        } while (!atomic_compare_exchange_weak(&page_bitmap[word_index], &expected, desired));
    }

    atomic_fetch_sub(&free_pages, size);
    return false;
}

static inline void page_dealloc_link_word(size_t word_index, uint8_t bit_index, uint8_t size) {
    if (!size)
        return;

    if (bit_index + size > BITMAP_WORD_BITS)
        return;

    bitmap_word mask = 0;
    for (uint8_t i = bit_index; i < bit_index + size; ++i) {
        mask |= ((bitmap_word)1 << i);
    }

    bitmap_word expected, desired;
    do {
        // We since nothing can deallocate things that are allocated but us we can just blindly try to
        // free the pages we want to free, if something else has changed while we were working on it we
        // just try again, only allocations/deallocations on different bits can be happening at the same
        // time.
        expected = atomic_load(&page_bitmap[word_index]);
        desired  = expected | mask;
    } while (!atomic_compare_exchange_weak(&page_bitmap[word_index], &expected, desired));

    atomic_fetch_add(&free_pages, size);
}

static size_t page_alloc_link_fit(size_t size, uint32_t start, uint32_t end, bool first_fit) {
    size_t start_index       = 0;
    size_t run               = 0;
    size                    += 2;
    size_t best_start_index  = 0;
    size_t best_run          = pages + 1;

    for (size_t i = start; i < end; i++) {
        if (start_index + (size - 1) > pages) {
            break;
        }

        if (first_fit && run >= size) {
            best_run         = run;
            best_start_index = start_index;
            break;
        }

        if (first_fit && best_run == size) {
            break;
        }

        bitmap_word word = atomic_load(&page_bitmap[i]);

        // printf("Checking word %i, run %zi, best_run %zi, used: ", i, run, best_run);

        // If all pages in the word are used the run ends
        if (word == 0) {
            // printf("Size: %zi, Word %i full, run %zi, best_run %zi\n", size, i, run, best_run);
            if (run >= size && run < best_run) {
                best_run         = run;
                best_start_index = start_index;
            }
            run = 0;
            continue;
        }

        // If the whole word is free just add it to the run
        if (word == BITMAP_WORD_MAX) {
            // printf("Word is empty (word_index: %i, current run: %zi)\n", i, run);
            if (!run) {
                start_index = i * BITMAP_WORD_BITS;
            }

            run += BITMAP_WORD_BITS;
            continue;
        }

        uint32_t set_bits = bitmap_count_set_bits(word);
        if (set_bits + run < size) {
            // There's not enough free pages in this word at all, no need to check each bit
            // printf("Word %i doesn't have enough free bits to complete run\n", i);

            // The starting free pages of this word form the start of the run
            run         = bitmap_count_leading_set_bits(word);
            start_index = (i * BITMAP_WORD_BITS) + (BITMAP_WORD_BITS - run);
            // printf("Run starts at leading word %i offset %i index %zi\n", i, run, start_index);
            continue;
        }

        // Check to see if the start of the word has enough bits to complete the run
        uint32_t trailing = bitmap_count_trailing_set_bits(word);
        if (run + trailing >= size) {
            // printf("Word %i has enough free bits at the start to complete run\n", i);
            if (!run) {
                start_index = (i * BITMAP_WORD_BITS);
            }
            run += trailing;
            continue;
        }

        // printf("Checking each bit in word %i, run %zi, longest_run %zi\n", i, run, longest_run);
        for (uint32_t bit_index = trailing + 1; bit_index < BITMAP_WORD_BITS; bit_index++) {
            if (!page_bitmap_get_bit(word, bit_index)) {
                // printf("Size: %zi, Run interrupted at %i:%i full, run %zi, best_run %zi\n", size, i, bit_index, run,
                // best_run);
                if (run >= size && run < best_run) {
                    best_run         = run;
                    best_start_index = start_index;
                }
                run = 0;
                continue;
            }

            if (!run) {
                start_index = (i * BITMAP_WORD_BITS) + bit_index;
            }

            ++run;
            if (first_fit && run >= size) {
                break;
            }
        }
        // printf("Finished checking each bit in word %i, run %zi, longest_run %zi\n", i, run, longest_run);
    }

    if (best_run > pages) {
        if (run >= size) {
            if (start_index + (size - 1) > pages) {
                return pages + 1;
            }

            best_start_index = start_index;
        } else {
            return pages + 1;
        }
    }

    // leave a page between each page_alloc_link allocation
    ++best_start_index;

    // printf("Best run for size %zi: %zi, start_index: %zi\n", size, best_run, best_start_index);
    return best_start_index;
}

void *page_alloc_link(size_t size) {
    if (!size)
        return NULL;

start_alloc:
    // We need to lock here because if multiple multi-page allocations run in parallel
    // the odds of someone stepping on our toes are very high.
    SPIN_LOCK_LOCK(link_lock);
    if (atomic_load(&free_pages) < size + 2)
        goto out;

    size_t start_index = page_alloc_link_fit(size, 0, (page_bitmap_size / BITMAP_WORD_BYTES) / 4, false);

    if (start_index > pages) {
        if (size >= (page_bitmap_size / BITMAP_WORD_BYTES) / 4) {
            start_index = page_alloc_link_fit(size, 0, (page_bitmap_size / BITMAP_WORD_BYTES), true);
        } else {
            start_index = page_alloc_link_fit(
                size,
                ((page_bitmap_size / BITMAP_WORD_BYTES) / 4) - size,
                (page_bitmap_size / BITMAP_WORD_BYTES),
                true
            );
        }

        if (start_index > pages) {
            goto out;
        }
    }

    char    *page       = get_page_by_index(start_index);
    uint32_t word_index = start_index / BITMAP_WORD_BITS;
    uint8_t  bit_index  = start_index % BITMAP_WORD_BITS;

    // printf("Ended search for run of length %zi, start_index: %zi, run: %zi, longest run: %zi, word_index: %i,
    // bit_index: %i\n", size + 2, start_index, run, longest_run, word_index, bit_index);

    if (size + bit_index < BITMAP_WORD_BITS) {
        // small single word allocation
        // just update the single word we need and retry if it fails
        if (page_alloc_link_word(word_index, bit_index, size)) {
            SPIN_LOCK_UNLOCK(link_lock);
            // printf("Atomic operation failed, restarting (simple) (%p)\n", page);
            goto start_alloc;
        }

    } else {
        // complex allocation
        // we need to span multiple words here, we do the following:
        // * Allocate the pages in the first partial word
        // * Allocate the pages in the final partial word
        // * Allocate all of the full words in between
        //
        // If any of these steps fail due to being raced by the page
        // allocator, undo all of the work and just start over
        size_t remaining    = size;
        size_t first_alloc  = BITMAP_WORD_BITS - bit_index;
        remaining          -= first_alloc;

        size_t full_words   = remaining / BITMAP_WORD_BITS;
        size_t last_alloc   = remaining % BITMAP_WORD_BITS;

        // first partial
        if (page_alloc_link_word(word_index, bit_index, first_alloc)) {
            SPIN_LOCK_UNLOCK(link_lock);
            goto start_alloc;
        }

        // final partial
        if (page_alloc_link_word(word_index + full_words + 1, 0, last_alloc)) {
            page_dealloc_link_word(word_index, bit_index, first_alloc);
            SPIN_LOCK_UNLOCK(link_lock);
            goto start_alloc;
        }

        // full word allocations
        for (size_t i = 0; i < full_words; ++i) {
            if (page_alloc_link_word(word_index + 1 + i, 0, BITMAP_WORD_BITS)) {
                // printf("Atomic operation failed, restarting (full page) (%p)\n", page);
                // back out all changes up until now
                page_dealloc_link_word(word_index, bit_index, first_alloc);
                page_dealloc_link_word(word_index + full_words + 1, 0, last_alloc);

                for (size_t k = 0; k < i; ++k) {
                    page_dealloc_link_word(word_index + 1 + k, 0, BITMAP_WORD_BITS);
                }
                SPIN_LOCK_UNLOCK(link_lock);
                goto start_alloc;
            }
        }
    }

    SPIN_LOCK_UNLOCK(link_lock);

    for (size_t i = start_index; i < start_index + size; ++i) {
        set_page_type_data(i, ALLOCATOR_PAGE_LINK, 0);
    }

    // printf("Allocated %zi pages, starting at %zi, pointer: (%p)\n", size, start_index, page);
    return page;

out:
    SPIN_LOCK_UNLOCK(link_lock);
    return NULL;
}

void page_free_link(void *ptr) {
    if (!ptr)
        return;

    size_t  page_index = get_page_index(ptr);
    uint8_t packed     = get_page_type_data(page_index);
    uint8_t type       = type_data_get_type(packed);

    if (type != ALLOCATOR_PAGE_LINK) {
#ifndef BADGEROS_KERNEL
        printf("page_free_link: attempting to free wrong page type (%p)\n", ptr);
        // fflush(stdout);
        // exit(1);
#endif
        return;
    }

    size_t dealloc_count = 0;
    for (size_t i = page_index; i < pages; ++i) {
        uint8_t page_packed = get_page_type_data(i);
        uint8_t page_type   = type_data_get_type(page_packed);

        // printf("Freeing page %zi, page_type: %i, page_cookie: %i\n", i, page_type, page_cookie);

        if (page_type == ALLOCATOR_PAGE_LINK) {
            set_page_type_data(i, ALLOCATOR_PAGE, 0);
            page_free(get_page_by_index(i));
            ++dealloc_count;
        } else {
            // printf("Freed %zi pages starting at %zi (%p)\n", dealloc_count, page_index, ptr);
            return;
        }
    }
    // printf("Something wrong trying to free %zi pages starting at %zi (%p)\n", dealloc_count, page_index, ptr);
}

void *page_alloc(enum allocator_type type, uint8_t data) {
start_alloc:
    if (!atomic_load(&free_pages))
        return NULL;

    for (uint32_t i = 0; i < page_bitmap_size / BITMAP_WORD_BYTES; ++i) {
        bitmap_word word = atomic_load(&page_bitmap[i]);
        if (word == 0)
            continue;

        uint32_t    bit_index = bitmap_find_first_trailing_set_bit(word);
        bitmap_word desired   = page_bitmap_clear_bit(word, bit_index);

        if (!atomic_compare_exchange_strong(&page_bitmap[i], &word, desired)) {
            // Allocation failed due to race condition. We will just start from the top
            // This can be made a bit more efficient by for instance only starting from
            // the current word, but we want to achieve tight packing of pages near
            // the start.
            goto start_alloc;
        }

        atomic_fetch_sub(&free_pages, 1);
        size_t page_index = i * BITMAP_WORD_BITS + bit_index;
        set_page_type_data(page_index, type, data);
        void *page = get_page_by_index(page_index);
        // printf("Allocated page: (%p)\n", page);
        return page;
    }
    return NULL;
}

void page_free(void *ptr) {
    if (!ptr) {
#ifndef BADGEROS_KERNEL
        printf("Attempting to free NULL\n");
#endif
        return;
    }

    size_t      page_index = get_page_index(ptr);
    uint32_t    word_index = page_index / BITMAP_WORD_BITS;
    uint8_t     bit_index  = page_index % BITMAP_WORD_BITS;

    bitmap_word expected, desired;
    do {
        expected = atomic_load(&page_bitmap[word_index]);
        desired  = page_bitmap_set_bit(expected, bit_index);

        if (desired == expected) {
#ifndef BADGEROS_KERNEL
            printf("Duplicate page_free() ptr: %p, %zi (%i, %i)\n", ptr, page_index, word_index, bit_index);
#endif
            return;
        }
    } while (!atomic_compare_exchange_weak_explicit(
        &page_bitmap[word_index],
        &expected,
        desired,
        memory_order_acq_rel,
        memory_order_acquire
    ));
    atomic_fetch_add(&free_pages, 1);
    // printf("Free page: (%p)\n", ptr);
    // printf("page_free: %zi\n", atomic_fetch_add(&free_pages, 1));
}
