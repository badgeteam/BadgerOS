#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define MAX_ORDER 8  // Assuming max number of pages is 2^32

typedef struct buddy_block {
    struct buddy_block* next;
    uint8_t order;
} buddy_block;

typedef struct {
    buddy_block* free_list[MAX_ORDER];
    void* mem_start;
    size_t total_pages;
} buddy_allocator;

size_t get_order(size_t size) {
    size_t order = 0;
    size -= 1;
    while (size >>= 1) order++;
    return order;
}

size_t init_buddy(size_t pages, void* mem) {
    memset(mem, 0, sizeof(buddy_allocator) + sizeof(buddy_block) * pages);

    buddy_allocator* allocator = (buddy_allocator*) mem;
    allocator->mem_start = mem + sizeof(buddy_allocator);
    allocator->total_pages = pages;

    size_t max_order = get_order(pages);
    buddy_block* block = (buddy_block*)allocator->mem_start;

    // Mark the blocks as free for the maximum possible order.
    for (size_t i = 0; i < pages; i += (1 << max_order)) {
        block->order = max_order;
        block->next = allocator->free_list[max_order];
        allocator->free_list[max_order] = block;
        block += (1 << max_order);
    }

    size_t block_size = sizeof(buddy_block);
    size_t allocator_size = sizeof(buddy_allocator);

    return block_size * pages + allocator_size;
}

size_t buddy_allocate(buddy_allocator* allocator, size_t size) {
    uint8_t order = get_order(size);
    uint8_t original_order = order;

    while (order < MAX_ORDER && !allocator->free_list[order]) order++;

    if (order == MAX_ORDER) return (size_t)-1;  // Not enough memory

    buddy_block* block = allocator->free_list[order];
    allocator->free_list[order] = block->next;

    while (order > original_order) {
        order--;
        buddy_block* buddy = (buddy_block*)(((char*)block) + (1 << order));
        buddy->next = allocator->free_list[order];
        buddy->order = order;
        allocator->free_list[order] = buddy;
        block->order = order;
    }

    return ((char*)block - (char*)allocator->mem_start) / sizeof(buddy_block);
}

void buddy_free(buddy_allocator* allocator, size_t start_index, size_t size) {
    buddy_block* block = (buddy_block*)((char*)allocator->mem_start + start_index * sizeof(buddy_block));
    uint8_t order = get_order(size);

    while (order < MAX_ORDER - 1) {
        size_t buddy_index = start_index ^ (1 << order);
        buddy_block* buddy = (buddy_block*)((char*)allocator->mem_start + buddy_index * sizeof(buddy_block));

        if (buddy->order != order || buddy != allocator->free_list[order]) break;

        if (buddy == allocator->free_list[order]) {
            allocator->free_list[order] = buddy->next;
        } else {
            buddy_block* tmp = allocator->free_list[order];
            while (tmp->next != buddy) tmp = tmp->next;
            tmp->next = buddy->next;
        }

        // Coalesce
        if (buddy_index < start_index) block = buddy;
        allocator->free_list[order] = buddy->next;
        order++;
    }

    block->next = allocator->free_list[order];
    block->order = order;
    allocator->free_list[order] = block;
}

void print_buddy(buddy_allocator* allocator) {
    for (size_t i = 0; i < MAX_ORDER; i++) {
        printf("Order %lu (Size %lu pages):", i, (size_t)1 << i);
        for (buddy_block* block = allocator->free_list[i];
             block && (char*)block >= (char*)allocator->mem_start &&
             (char*)block < (char*)allocator->mem_start + allocator->total_pages * sizeof(buddy_block);
             block = block->next) {
            printf(" %lu", ((char*)block - (char*)allocator->mem_start) / sizeof(buddy_block));
        }
        printf("\n");
    }
}
