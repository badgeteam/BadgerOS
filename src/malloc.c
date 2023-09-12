
// SPDX-License-Identifier: MIT

#include <stddef.h>

#ifdef BADGEROS_KERNEL
#include "log.h"

extern char __start_free_sram[];
extern char __stop_free_sram[];

#define __wrap_malloc       malloc
#define __wrap_free         free
#define __wrap_calloc       calloc
#define __wrap_realloc      realloc
#define __wrap_reallocarray reallocarray

#else
#include <stdio.h>
#include <stdlib.h>

void *__real_malloc(size_t size);
void  __real_free(void *ptr);
void *__real_calloc(size_t nmemb, size_t size);
void *__real_realloc(void *ptr, size_t size);
void *__real_reallocarray(void *ptr, size_t nmemb, size_t size);
#endif

#define NUM_SIZE_CLASSES 5
#define MBLK_SIZE        ALIGN(sizeof(free_blk_header_t))

#define ALIGNMENT   8
#define ALIGN(size) (((size) + (ALIGNMENT - 1)) & ~(ALIGNMENT - 1))

typedef struct free_blk_header {
    size_t                  size;
    struct free_blk_header *next;
    struct free_blk_header *prior;
} free_blk_header_t;

char              *mem_start;
char              *mem_end;
char              *mem_end_max;
free_blk_header_t *free_lists;

void  kernel_heap_init();
void  try_split(free_blk_header_t *fp, size_t needed);
void *find_fit(size_t size);

size_t min_class_size[] = {MBLK_SIZE, 64, 128, 256, 1024};

#ifndef BADGEROS_KERNEL
void print_heap() {
    for (int i = 0; i < NUM_SIZE_CLASSES; i++) {
        printf("Bucket size: %zi\n", min_class_size[i]);
        free_blk_header_t *fp = &free_lists[i];

        while (fp->next != &free_lists[i]) {
            printf("\tFree block of size: %zi\n", fp->next->size);
            fp = fp->next;
        }
    }
}
#else
void  print_heap() {
    for (int i = 0; i < NUM_SIZE_CLASSES; i++) {
        logkf(LOG_DEBUG, "Bucket size: %{size;d}", min_class_size[i]);
        free_blk_header_t *fp = &free_lists[i];

        while (fp->next != &free_lists[i]) {
            logkf(LOG_DEBUG, "\tFree block 0x%{size;x} of size: %{size;d}", fp->next, fp->next->size);
            fp = fp->next;
        }
    }
}
#endif

void kernel_heap_init() {
#ifdef BADGEROS_KERNEL
    free_lists  = (free_blk_header_t *)__start_free_sram;
    mem_end_max = __stop_free_sram;
#else
    free_lists  = __real_malloc(1024 * 1024 * 4);
    mem_end_max = ((char *)free_lists) + 1024 * 1024 * 4;
#endif

    mem_start = ((char *)free_lists) + (NUM_SIZE_CLASSES * sizeof(free_blk_header_t));
    mem_end   = mem_start;

    for (int i = 0; i < NUM_SIZE_CLASSES; i++) {
        free_lists[i].size = 0;
        free_lists[i].next = free_lists[i].prior = &free_lists[i];
    }
}

void *find_fit(size_t size) {
    free_blk_header_t *fp;

    for (int i = 0; i < NUM_SIZE_CLASSES; i++) {
        if (min_class_size[i] >= size && free_lists[i].next != &free_lists[i]) {
            fp                 = free_lists[i].next;
            free_lists[i].next = fp->next;
            fp->next->prior    = &free_lists[i];
            logkf(LOG_DEBUG, "0x%{size;x} %{size;d}", fp, fp->size);
            try_split(fp, size);
            return fp;
        }
    }

    return NULL;
}

void try_split(free_blk_header_t *fp, size_t needed) {
    if (fp->size < needed)
        return;
    size_t             remaining = fp->size - needed;
    free_blk_header_t *sp;

    if (remaining < MBLK_SIZE)
        return;

    fp->size = needed;
    sp       = (free_blk_header_t *)((char *)fp + needed);
    sp->size = remaining;

    for (int i = NUM_SIZE_CLASSES - 1; i > 0; i--) {
        if (min_class_size[i] <= remaining) {
            sp->prior          = &free_lists[i];
            sp->next           = free_lists[i].next;
            free_lists[i].next = free_lists[i].next->prior = sp;
            break;
        }
    }
}

void *__wrap_malloc(size_t size) {
    size_t *header;
    size_t  blk_size = ALIGN(size + sizeof(free_blk_header_t));

    blk_size = (blk_size < MBLK_SIZE) ? MBLK_SIZE : blk_size;
    header   = find_fit(blk_size);

    if (header) {
        *header = ((free_blk_header_t *)header)->size | 1;
    } else {
        if (mem_end + blk_size > mem_end_max)
            return NULL;

        header   = (size_t *)mem_end;
        mem_end += blk_size;
        *header  = blk_size | 1;
    }
    return (char *)header + sizeof(free_blk_header_t);
}

void __wrap_free(void *ptr) {
    if (!ptr)
        return;

    free_blk_header_t *header = (free_blk_header_t *)((char *)ptr - sizeof(free_blk_header_t));
    size_t             size   = header->size;
    header->size              = *(size_t *)header & ~1L;

    for (int i = NUM_SIZE_CLASSES - 1; i >= 0; i--) {
        if (min_class_size[i] <= size) {
            header->next       = free_lists[i].next;
            header->prior      = &free_lists[i];
            free_lists[i].next = free_lists[i].next->prior = header;
            return;
        }
    }
}

void *__wrap_realloc(void *ptr, size_t size) {
    if (!ptr && size) {
        return malloc(size);
    }

    if (ptr && !size) {
        free(ptr);
        return NULL;
    }

    free_blk_header_t *header   = (free_blk_header_t *)((char *)ptr - sizeof(free_blk_header_t));
    size_t             old_size = header->size & ~1L;

    char *new_ptr = malloc(size);
    if (!new_ptr) {
        return NULL;
    }

    size_t copy_size = old_size < size ? old_size : size;
    __builtin_memcpy(new_ptr, ptr, copy_size);
    free(ptr);
    return new_ptr;
}
