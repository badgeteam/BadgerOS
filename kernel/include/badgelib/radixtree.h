
// SPDX-FileCopyrightText: 2025 Julian Scheffers <julian@scheffers.net>
// SPDX-FileType: SOURCE
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>



// A generic radix tree.
typedef struct rtree      rtree_t;
// A radix tree node.
typedef struct rtree_node rtree_node_t;
// A radix tree iterator.
typedef struct rtree_iter rtree_iter_t;

// A generic radix tree.
struct rtree {
    // Size of the data in the tree.
    size_t        value_size;
    // Number of leaf nodes in the tree.
    size_t        len;
    // Total width of the key in bits, at most 64.
    uint8_t       key_width;
    // Bits consumed per node, at most 8.
    uint8_t       bits_per_node;
    // Tree root node.
    rtree_node_t *root;
};

// A radix tree node.
struct rtree_node {
    // Extra key match bits.
    uint32_t subkey;
    // Number of bits in the subkey to match; must be a multiple of the tree's bits_per_node.
    uint8_t  subkey_bits;
    // Number of child nodes present minus one; irrelevant for leaf nodes.
    uint8_t  len;
    // Is a leaf node?
    bool     is_leaf;
    // Padding.
    uint8_t : 8;
    // Child node pointers or a single data pointer.
    uint8_t data[];
};

// A radix tree iterator.
struct rtree_iter {
    // Iterator is valid.
    bool     valid;
    // Current key.
    uint64_t key;
    // Current value.
    void    *value;
};

// Initialize an empty radix tree of pointers.
#define RTREE_T_INIT(_value_size, _key_width, _bits_per_node) {_value_size, 0, _key_width, _bits_per_node, NULL}

// Iterate a radix tree by key and value.
#define rtree_foreach(iter_name, tree)                                                                                 \
    for (rtree_iter_t iter_name = rtree_next((tree), NULL); iter_name.valid; iter_name = rtree_next((tree), &iter_name))



// Get a value from the radix tree.
void        *rtree_get(rtree_t const *tree, uint64_t key);
// Insert a value into the radix tree.
bool         rtree_set(rtree_t *tree, uint64_t key, void *value);
// Remove an entry from the radix tree.
bool         rtree_remove(rtree_t *tree, uint64_t key);
// Clear the radix tree.
void         rtree_clear(rtree_t *tree);
// Get the next entry in the radix tree.
// If `iter` is `NULL`, the first entry is returned.
rtree_iter_t rtree_next(rtree_t const *tree, rtree_iter_t *cur);
// Debug-print a radix tree.
void         rtree_dump(rtree_t const *tree, void (*printer)(int indent, void *value));
