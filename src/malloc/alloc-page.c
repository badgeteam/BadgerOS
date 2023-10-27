#include "alloc-page.h"

#include "allocator.h"
#include "compiler.h"
#include "debug.h"
#include "util.h"

#include "skiplist-impl.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

page_alloc_state_t page_alloc_state = {0};
page_pool_t       *user_pool        = {0};
page_pool_t       *kernel_pool      = {0};

/* clang-format */

void kernel_page_alloc_free(size_t index, size_t size) {
    //size_t page_index = (ptr - kernel_pool->usable_pages_start) / PAGE_SIZE;
    //printf("Freeing page %zi\n", page_index);
    
    skiplist_insert(&kernel_pool->pages_list, index, size);
}

static void set_kernel_split(void *ptr) {
    (void)ptr;
    BADGEROS_MALLOC_MSG_INFO("Setting kernel split to: %p", ptr);
}

static void page_pool_set_pages(page_pool_t *pool, size_t pages) {
    size_t page_list_size = sizeof(skiplist_node_t) * page_alloc_state.pages;
    size_t page_list_pages = (page_list_size + (PAGE_SIZE - 1)) / PAGE_SIZE;

    atomic_store(&pool->pages, pages);
    atomic_store(&pool->usable_pages, pages - page_list_pages);

    pool->pages_list.nodes = pool->pages_start;
    pool->pages_list.size = pages - page_list_pages;
    pool->usable_pages_start = ALIGN_PAGE_UP(pool->pages_start + page_list_size);

    for (size_t i = 0; i < pool->pages_list.size; ++i) {
        atomic_flag_clear(&pool->pages_list.nodes[i].modifying);
	pool->pages_list.nodes[i].size = 0;
    }
    pool->pages_list.head_index.height = SKIPLIST_MAX_HEIGHT;
    pool->pages_list.head_size.height = SKIPLIST_MAX_HEIGHT;

    //skiplist_insert(&pool->pages_list, 0, pool->usable_pages);

    if (pool->grows_up) {
        atomic_store(&pool->pages_end, (uintptr_t)(pool->pages_start + (pages * PAGE_SIZE)));
    } else {
        atomic_store(&pool->pages_end, (uintptr_t)(pool->pages_start - (pages * PAGE_SIZE)));
    }
}

static void page_pool_init(page_pool_t *pool, bool grows_up) {
    pool->grows_up = grows_up;

    if (grows_up) {
        pool->pages_start = page_alloc_state.pages_start;
    } else {
        pool->pages_start = page_alloc_state.pages_end - PAGE_SIZE;
    }
}

void page_alloc_init(void *start, void *end) {
    user_pool                       = (void *)ALIGN_UP(start, 8);
    kernel_pool                     = (void *)(end - ALIGN_UP(sizeof(page_pool_t), 8));

    page_alloc_state.mem_start      = start;
    page_alloc_state.mem_end        = end;
    page_alloc_state.total_mem_size = (size_t)end - (size_t)start;
    page_alloc_state.pages_start    = ALIGN_PAGE_UP(user_pool);
    page_alloc_state.pages_end      = ALIGN_PAGE_DOWN(kernel_pool);
    page_alloc_state.pages =
        (((size_t)page_alloc_state.pages_end) - ((size_t)page_alloc_state.pages_start)) / PAGE_SIZE;

    //size_t initial_user_size   = page_alloc_state.pages / 8;
    //size_t initial_kernel_size = page_alloc_state.pages / 8;
    size_t initial_user_size   = 0;
    size_t initial_kernel_size = page_alloc_state.pages;

    if (initial_user_size) {
        page_pool_init(user_pool, false);
        page_pool_set_pages(user_pool, initial_user_size);
    }

    if (initial_kernel_size) {
        page_pool_init(kernel_pool, true);

        page_pool_set_pages(kernel_pool, initial_kernel_size);
    }

    set_kernel_split((void *)atomic_load(&user_pool->pages_end));

    BADGEROS_MALLOC_MSG_INFO(
        "Memory_start      : %p, memory_end: %p, total size: %zi, total pages: %zi",
        start,
        end,
        page_alloc_state.total_mem_size,
        page_alloc_state.pages
    );
    BADGEROS_MALLOC_MSG_INFO(
        "User   page_pool: %p pages_start  : %p, pages_end: %p, pages: %zi",
        user_pool,
        user_pool->pages_start,
        (void *)user_pool->pages_end,
        user_pool->pages
    );
    BADGEROS_MALLOC_MSG_INFO(
        "Kernel page_pool: %p pages: %zi, usable pages: %zi, overhead: %.02f%%",
        kernel_pool,
        kernel_pool->pages,
        kernel_pool->usable_pages,
	(((float)kernel_pool->pages - (float)kernel_pool->usable_pages) / (float)kernel_pool->pages) * 100.0
    );
    if (initial_user_size) {
        BADGEROS_MALLOC_MSG_INFO(
            "User   first page: %p, last page: %p",
            get_page_address(user_pool, 0),
            get_page_address(user_pool, user_pool->pages - 1)
        );
    }
    BADGEROS_MALLOC_MSG_INFO(
        "Kernel first page: %p, last page: %p",
        get_page_address(kernel_pool, 0),
        get_page_address(kernel_pool, kernel_pool->pages - 1)
    );
}
