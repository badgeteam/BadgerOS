#pragma once

#include "bitops.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ALIGN_UP(x, y) (void *)(((size_t)(x) + y - 1) & ~(y - 1))

#define ALIGN_PAGE_UP(x)   ALIGN_UP(x, PAGE_SIZE)
#define ALIGN_PAGE_DOWN(x) (void *)((size_t)(x) & ~(PAGE_SIZE - 1))

#define SPIN_LOCK_LOCK(x)                                                                                              \
    while (atomic_flag_test_and_set_explicit(&x, memory_order_acquire)) {                                              \
    }
#define SPIN_LOCK_TRY_LOCK(x) !atomic_flag_test_and_set_explicit(&x, memory_order_acquire)
#define SPIN_LOCK_UNLOCK(x) atomic_flag_clear_explicit(&x, memory_order_release)

#define PAGE_SIZE 4096

#ifndef BITMAP_WORD_BITS
#if defined(__GCC_HAVE_SYNC_COMPARE_AND_SWAP_8)
#define BITMAP_WORD_BITS 64
#pragma message "Selected atomic width: 64 bits"
#elif defined(__GCC_HAVE_SYNC_COMPARE_AND_SWAP_4)
#define BITMAP_WORD_BITS 32
#pragma message "Selected atomic width: 32 bits"
#else
#error "Unsupported atomic width!"
#endif
#endif

#define CONCAT_INNER(a, b) a##b
#define CONCAT(a, b)       CONCAT_INNER(a, b)

#define BITMAP_WORD_TYPE(bits)        CONCAT(uint, CONCAT(bits, _t))
#define BITMAP_WORD_TYPE_ATOMIC(bits) CONCAT(atomic_uint_least, CONCAT(bits, _t))
#define UINT_MAX_FOR_BITS(bits)       CONCAT(UINT, CONCAT(bits, _MAX))

typedef BITMAP_WORD_TYPE(BITMAP_WORD_BITS) bitmap_word;
typedef BITMAP_WORD_TYPE_ATOMIC(BITMAP_WORD_BITS) bitmap_word_atomic;
#define BITMAP_WORD_BYTES sizeof(bitmap_word)
#define BITMAP_WORD_MAX   UINT_MAX_FOR_BITS(BITMAP_WORD_BITS)

#ifndef BADGEROS_KERNEL
#include <stdio.h>

static inline void print_word(const bitmap_word word) {
    for (uint8_t x = 0; x < BITMAP_WORD_BITS; ++x) {
        printf("%i", (word & ((bitmap_word)1 << x)) != 0);
    }
    printf("\n");
}
#endif

static inline uint32_t bitmap_count_set_bits(const bitmap_word word) {
    return CONCAT(count_set_bits, BITMAP_WORD_BITS)(word);
}
static inline uint32_t bitmap_count_trailing_set_bits(const bitmap_word word) {
    return CONCAT(count_trailing_set_bits, BITMAP_WORD_BITS)(word);
}
static inline uint32_t bitmap_count_trailing_unset_bits(const bitmap_word word) {
    return CONCAT(count_trailing_unset_bits, BITMAP_WORD_BITS)(word);
}
static inline uint32_t bitmap_count_leading_set_bits(const bitmap_word word) {
    return CONCAT(count_leading_set_bits, BITMAP_WORD_BITS)(word);
}
static inline uint32_t bitmap_count_leading_unset_bits(const bitmap_word word) {
    return CONCAT(count_leading_unset_bits, BITMAP_WORD_BITS)(word);
}
static inline uint32_t bitmap_find_first_trailing_set_bit(const bitmap_word word) {
    return CONCAT(find_first_trailing_set_bit, BITMAP_WORD_BITS)(word);
}

enum slab_sizes { SLAB_SIZE_32 = 0, SLAB_SIZE_64 = 1, SLAB_SIZE_128 = 2, SLAB_SIZE_256 = 3 };
static const uint16_t slab_sizes[] = {32, 64, 128, 256};

enum allocator_type { ALLOCATOR_PAGE = 0, ALLOCATOR_SLAB = 1, ALLOCATOR_BUDDY = 2, ALLOCATOR_PAGE_LINK = 3 };

#ifndef BADGEROS_KERNEL
#include <assert.h>

static_assert((ALLOCATOR_PAGE_LINK < 16), "Allocator type values must fit into 4 bits");
#endif

void         page_alloc_init(void *start, void *end);
size_t       get_free_pages();
size_t       get_pages();
bitmap_word *get_page_bitmap();
size_t       get_page_bitmap_size();

size_t       get_page_index(void *ptr);
size_t       get_page_index_by_type_data(size_t start_index, enum allocator_type type, uint8_t data);
void        *get_page_by_index(size_t index);
uint8_t      get_page_type(size_t index);
uint8_t      get_page_data(size_t index);

void        *page_alloc(enum allocator_type type, uint8_t data);
void         page_free(void *ptr);
uint8_t      is_page_free(void *ptr);

void        *page_alloc_link(size_t size);
void         page_free_link(void *ptr);

void        *slab_alloc(size_t size);
void         slab_free(void *ptr);
void         deallocate_inactive();
