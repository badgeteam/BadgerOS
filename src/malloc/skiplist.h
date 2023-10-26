#pragma once

#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>

#include "compiler.h"
#include "debug.h"

#define SKIPLIST_MAX_HEIGHT 8

typedef struct {
    uint16_t next_index[SKIPLIST_MAX_HEIGHT];
    uint16_t prev_index[SKIPLIST_MAX_HEIGHT];
    uint16_t next_size[SKIPLIST_MAX_HEIGHT];
    uint16_t prev_size[SKIPLIST_MAX_HEIGHT];

    uint16_t size;
    uint16_t height; // determined by a hash function

    atomic_flag modifying;
} skiplist_node_t;

typedef struct {
    skiplist_node_t head_index;
    skiplist_node_t head_size;
    skiplist_node_t *nodes; // Page 0 is our no-next-page marker
    size_t size;
} skiplist_t;

void skiplist_initialize(skiplist_t* list, size_t node_count);
