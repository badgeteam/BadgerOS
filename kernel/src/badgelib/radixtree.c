
// SPDX-FileCopyrightText: 2025 Julian Scheffers <julian@scheffers.net>
// SPDX-FileType: SOURCE
// SPDX-License-Identifier: MIT

#include "radixtree.h"

#include "badge_strings.h"
#include "malloc.h"
#include "rawprint.h"

#include <stdint.h>



// Get a value from the radix tree.
void *rtree_get(rtree_t const *tree, uint64_t key) {
    key <<= 64 - tree->key_width;

    uint8_t             remaining_key = tree->key_width;
    rtree_node_t const *node          = tree->root;
    while (node) {
        // Match against the subkey optimization.
        uint64_t subkey_mask = (1llu << node->subkey_bits) - 1;
        if (node->subkey != ((key >> (64 - tree->bits_per_node)) & subkey_mask)) {
            return false;
        }
        key <<= node->subkey_bits;

        // If we arrive at a leaf node, we've found the data.
        if (node->is_leaf) {
            return (void *)node->data;
        }

        // Otherwise, go to next level of tree.
        uint8_t              layer_bits   = remaining_key < tree->bits_per_node ? remaining_key : tree->bits_per_node;
        rtree_node_t const **children     = (void *)node->data;
        node                              = children[key >> (64 - layer_bits)];
        key                             <<= layer_bits;
    }

    return NULL;
}

// Try to create a new node for the radix tree.
static rtree_node_t *rtree_create_node(rtree_t *tree, uint64_t key, uint8_t remaining_key, void *value) {
    uint8_t max_subkey_len = 32 - 32 % tree->bits_per_node;
    if (remaining_key <= max_subkey_len) {
        // Create leaf node.
        rtree_node_t *node = calloc(1, sizeof(rtree_node_t) + tree->value_size);
        node->subkey_bits  = remaining_key <= max_subkey_len ? remaining_key : max_subkey_len;
        node->subkey       = key >> (64 - node->subkey_bits);
        node->is_leaf      = true;
        mem_copy(&node->data, value, tree->value_size);
        return node;
    }

    // Take this layer's bits from the key.
    uint8_t       layer_bits = remaining_key < tree->bits_per_node ? remaining_key : tree->bits_per_node;
    rtree_node_t *node       = calloc(1, sizeof(rtree_node_t) + sizeof(void *) * (1 << layer_bits));
    node->subkey_bits =
        remaining_key - tree->bits_per_node <= max_subkey_len ? remaining_key - tree->bits_per_node : max_subkey_len;
    node->subkey    = key >> (64 - node->subkey_bits);
    key           <<= node->subkey_bits;
    remaining_key  -= node->subkey_bits;

    // Create child node.
    rtree_node_t const *child = rtree_create_node(tree, key << layer_bits, remaining_key - layer_bits, value);
    if (!child) {
        free(node);
        return NULL;
    }
    rtree_node_t const **children      = (void *)node->data;
    children[key >> (64 - layer_bits)] = child;

    return node;
}

// Try to split a leaf node at a certain bit, creating a new node in the process.
static rtree_node_t *
    rtree_split_node(rtree_t *tree, rtree_node_t *leaf, uint8_t identical_bits, uint8_t remaining_key) {
    if (identical_bits == remaining_key) {
        identical_bits -= tree->bits_per_node;
    }
    remaining_key -= identical_bits;

    // Create inner node.
    uint8_t       layer_bits = remaining_key < tree->bits_per_node ? remaining_key : tree->bits_per_node;
    rtree_node_t *inner      = calloc(1, sizeof(rtree_node_t) + sizeof(void *) * (1llu << layer_bits));
    if (!inner) {
        return NULL;
    }
    inner->subkey      = leaf->subkey >> (leaf->subkey_bits - identical_bits);
    inner->subkey_bits = identical_bits;

    // Insert leaf into new inner node.
    rtree_node_t **children = (void *)inner->data;
    size_t child_idx = (leaf->subkey >> (leaf->subkey_bits - identical_bits - layer_bits)) & ((1llu << layer_bits) - 1);
    children[child_idx] = leaf;

    // Update the leaf node's subkey.
    leaf->subkey_bits -= identical_bits + layer_bits;
    leaf->subkey      &= (1llu << leaf->subkey_bits) - 1;

    return inner;
}

// Insert a value into the radix tree.
bool rtree_set(rtree_t *tree, uint64_t key, void *value) {
    key <<= 64 - tree->key_width;

    if (tree->root == NULL) {
        // Edge case: empty tree.
        tree->root = rtree_create_node(tree, key, tree->key_width, value);
        tree->len  = 1;
        return tree->root != NULL;
    }

    uint8_t        remaining_key = tree->key_width;
    rtree_node_t **parent_ptr    = &tree->root;
    rtree_node_t  *node          = tree->root;
    while (1) {
        // Match against the subkey optimization.
        uint64_t subkey_mask = -(1llu << (64 - node->subkey_bits));
        uint64_t subkey_diff = (key & subkey_mask) ^ ((uint64_t)node->subkey << (64 - node->subkey_bits));
        if (subkey_diff) {
            // Determine how many upper bits are identical.
            uint8_t identical_bits  = __builtin_clzll(subkey_diff);
            identical_bits         -= identical_bits % tree->bits_per_node;

            // Split node.
            rtree_node_t *inner = rtree_split_node(tree, node, identical_bits, remaining_key);
            if (!inner) {
                return false;
            }
            *parent_ptr     = inner;
            key           <<= inner->subkey_bits;
            remaining_key  -= inner->subkey_bits;

            // Create new leaf node.
            uint8_t       layer_bits = remaining_key < tree->bits_per_node ? remaining_key : tree->bits_per_node;
            rtree_node_t *new_leaf   = rtree_create_node(tree, key << layer_bits, remaining_key - layer_bits, value);
            if (!new_leaf) {
                return false;
            }
            rtree_node_t **children            = (void *)inner->data;
            children[key >> (64 - layer_bits)] = new_leaf;
            inner->len++;
            tree->len++;

            return true;
        }
        key           <<= node->subkey_bits;
        remaining_key  -= node->subkey_bits;

        // If we arrive at a leaf node, we've found the data.
        if (node->is_leaf) {
            mem_copy(&node->data, value, tree->value_size);
            return true;
        }

        // Otherwise, go to next level of tree.
        uint8_t        layer_bits = remaining_key < tree->bits_per_node ? remaining_key : tree->bits_per_node;
        rtree_node_t **children   = (void *)node->data;
        parent_ptr                = &children[key >> (64 - layer_bits)];

        if (!*parent_ptr) {
            // Empty; create new leaf node.
            rtree_node_t *leaf = rtree_create_node(tree, key << layer_bits, remaining_key - layer_bits, value);
            if (!leaf) {
                return false;
            }
            *parent_ptr = leaf;
            node->len++;
            tree->len++;
            return true;
        }

        node            = *parent_ptr;
        key           <<= layer_bits;
        remaining_key  -= layer_bits;
    }
}

// Remove an entry from the radix tree.
// Returns 1 if successful, 2 if successful and the child node was deleted.
static int rtree_remove_impl(
    rtree_t *tree, rtree_node_t *parent_node, rtree_node_t *node, uint64_t key, uint8_t remaining_key
) {
    if (!node) {
        return 0;
    }
    if (key >> (64 - node->subkey_bits) != node->subkey) {
        return 0;
    }
    key           <<= node->subkey_bits;
    remaining_key  -= node->subkey_bits;

    if (node->is_leaf) {
        // Remove leaf node.
        free(node);
        tree->len--;
        return 2;
    }

    // Try to remove from child node.
    uint8_t        level_bits = remaining_key < tree->bits_per_node ? remaining_key : tree->bits_per_node;
    rtree_node_t **children   = (void *)&node->data;
    int            res        = rtree_remove_impl(
        tree,
        node,
        children[key >> (64 - level_bits)],
        key << level_bits,
        remaining_key - level_bits
    );
    if (!res) {
        return 0;
    }

    if (res == 2) {
        // Child node was removed.
        if (node->len == 0) {
            // This node must also be deleted.
            free(node);
            return 2;
        } else {
            // There are other children so this node will persist.
            children[key >> (64 - level_bits)] = NULL;
            node->len--;
        }
    }

    return 1;
}

// Remove an entry from the radix tree.
bool rtree_remove(rtree_t *tree, uint64_t key) {
    int res = rtree_remove_impl(tree, NULL, tree->root, key, tree->key_width);
    if (res == 2) {
        tree->root = NULL;
    }
    return res;
}

// Clear the radix tree.
static void rtree_clear_impl(rtree_t *tree, rtree_node_t *node, uint8_t remaining_key) {
    if (!node) {
        return;
    }
    remaining_key -= node->subkey_bits;
    if (!node->is_leaf) {
        uint8_t        level_bits = remaining_key < tree->bits_per_node ? remaining_key : tree->bits_per_node;
        rtree_node_t **children   = (void *)&node->data;
        for (int i = 0; i < 1 << level_bits; i++) {
            rtree_clear_impl(tree, children[i], remaining_key - level_bits);
        }
    }
    free(node);
}

// Clear the radix tree.
void rtree_clear(rtree_t *tree) {
    rtree_clear_impl(tree, tree->root, tree->key_width);
    tree->root = NULL;
}

// Get the first entry in the radix tree.
static rtree_iter_t
    rtree_first_impl(rtree_t const *tree, rtree_node_t const *node, uint64_t key, uint8_t remaining_key) {
    while (1) {
        if (!node) {
            return (rtree_iter_t){0};
        }

        key           <<= node->subkey_bits;
        key            |= node->subkey;
        remaining_key  -= node->subkey_bits;

        if (node->is_leaf) {
            return (rtree_iter_t){
                .valid = true,
                .key   = key,
                .value = (void *)&node->data,
            };
        }

        uint8_t              level_bits = remaining_key < tree->bits_per_node ? remaining_key : tree->bits_per_node;
        rtree_node_t const **children   = (void *)&node->data;
        node                            = NULL;
        for (int i = 0; i < 1 << level_bits; i++) {
            if (children[i]) {
                node            = children[i];
                key           <<= level_bits;
                key            |= i;
                remaining_key  -= level_bits;
                break;
            }
        }
    }
}

// Get the next entry in the radix tree.
// Returns 0 on failure, 1 on success, 2 if the next sibling to `node` is needed.
static int rtree_next_impl(
    rtree_t const      *tree,
    rtree_node_t const *node,
    rtree_iter_t       *next_out,
    uint64_t const      orig_key,
    uint64_t            key,
    uint8_t             remaining_key
) {
    if (!node) {
        return 0;
    }
    if (key >> (64 - node->subkey_bits) != node->subkey) {
        return 0;
    }
    key           <<= node->subkey_bits;
    remaining_key  -= node->subkey_bits;

    if (node->is_leaf) {
        // Target node found; indicate that the next sibling is needed.
        return 2;
    }

    // Try to find the next among children.
    uint8_t        level_bits = remaining_key < tree->bits_per_node ? remaining_key : tree->bits_per_node;
    rtree_node_t **children   = (void *)&node->data;
    int            child_idx  = key >> (64 - level_bits);
    if (!children[child_idx]) {
        return 0;
    }
    int res =
        rtree_next_impl(tree, children[child_idx], next_out, orig_key, key << level_bits, remaining_key - level_bits);
    if (res != 2) {
        return res;
    }

    // The child node wants its next sibling.
    for (int i = child_idx + 1; i < 1 << level_bits; i++) {
        if (children[i]) {
            key             = orig_key >> (64 - tree->key_width + remaining_key);
            key           <<= level_bits;
            key            |= i;
            remaining_key  += level_bits;
            *next_out       = rtree_first_impl(tree, children[i], key, remaining_key);
            return 1;
        }
    }

    // There is no next child node; want this node's next sibling.
    return 2;
}

// Get the next entry in the radix tree.
// If `iter` is `NULL`, the first entry is returned.
rtree_iter_t rtree_next(rtree_t const *tree, rtree_iter_t *cur) {
    if (cur == NULL) {
        return rtree_first_impl(tree, tree->root, 0, tree->key_width);
    }
    rtree_iter_t iter = {0};
    int          res  = rtree_next_impl(tree, tree->root, &iter, cur->key, cur->key, tree->key_width);
    if (res == 0 || res == 2) {
        return (rtree_iter_t){0};
    }
    return iter;
}

// Default print function for `rtree_dump`.
static void print_ptr(int indent, void *ptr) {
    rawprint("0x");
    rawprinthex((size_t)ptr, 2 * sizeof(size_t));
    rawputc('\n');
}

// Debug-print a radix tree.
static void rtree_dump_impl(
    int                 indent,
    rtree_t const      *tree,
    rtree_node_t const *node,
    uint8_t             remaining_key,
    void (*printer)(int indent, void *value)
) {
    if (node->subkey_bits) {
        rawputc('[');
        rawputc('0');
        rawputc('x');
        rawprinthex(node->subkey, (node->subkey_bits + 3) / 4);
        rawputc(':');
        rawprintudec(node->subkey_bits, 1);
        rawputc(']');
        rawputc(' ');
    }

    if (node->is_leaf) {
        printer(indent + 1, (void *)&node->data);
        return;
    }

    rawputc('\n');
    uint8_t              level_bits = remaining_key < tree->bits_per_node ? remaining_key : tree->bits_per_node;
    rtree_node_t const **children   = (void *)&node->data;
    for (int i = 0; i < 1 << level_bits; i++) {
        if (!children[i]) {
            continue;
        }

        for (int x = 0; x < indent; x++) {
            rawputc(' ');
            rawputc(' ');
        }
        rawputc('[');
        rawputc('0');
        rawputc('x');
        rawprinthex(i, (level_bits + 3) / 4);
        rawputc(':');
        rawprintudec(level_bits, 1);
        rawputc(']');
        rawputc(' ');

        rtree_dump_impl(indent + 1, tree, children[i], remaining_key - level_bits, printer);
    }
}

// Debug-print a radix tree.
void rtree_dump(rtree_t const *tree, void (*printer)(int indent, void *value)) {
    printer = printer ?: print_ptr;
    if (!tree->len) {
        rawprint("(empty rtree)\n");
        return;
    }
    rawprint("[root] ");
    rtree_dump_impl(1, tree, tree->root, tree->key_width, printer);
}
