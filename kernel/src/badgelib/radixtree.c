
// SPDX-FileCopyrightText: 2025 Julian Scheffers <julian@scheffers.net>
// SPDX-FileType: SOURCE
// SPDX-License-Identifier: MIT

#include "radixtree.h"

#include "cpu/interrupt.h"
#include "errno.h"
#include "malloc.h"
#include "rawprint.h"
#include "spinlock.h"
#include "time.h"
#include "todo.h"

#include <stdatomic.h>
#include <stdint.h>



// Initiate a stack or dynamically allocated radix tree.
void rtree_init(rtree_t *tree) {
    *tree = (rtree_t)RTREE_T_INIT;
    atomic_thread_fence(memory_order_release);
}

// Get a value from the radix tree.
void *rtree_get(rtree_t *tree, uint64_t key) {
    rtree_node_t *cur = atomic_load_explicit(&tree->root, memory_order_acquire);
    if (!cur || (cur->height < 64 && (key >> cur->height) >= RTREE_BITS_PER_NODE)) {
        // No content or the key is not covered by the root node.
        return NULL;
    }

    while (cur) {
        size_t index = (key >> cur->height) % RTREE_ENTS_PER_NODE;
        if (cur->height == 0) {
            return atomic_load_explicit(&cur->data[index], memory_order_acquire);
        } else {
            cur = atomic_load_explicit(&cur->children[index], memory_order_acquire);
        }
    }

    return NULL;
}

// Garbage-collect nodes along the path to `key`.
static void rtree_gc_key(rtree_t *tree, uint64_t key) {
}

// Create a new span of the radix tree from `max_height` downto 0, along the path for `key`.
// If successful, `value` will be stored at `key` in this subtree.
static rtree_node_t *rtree_create_subtree(uint64_t key, void *value, uint8_t max_height) {
    rtree_node_t *node = calloc(1, sizeof(rtree_node_t));
    if (!node) {
        // Allocation failed, return NULL.
        return NULL;
    }
    node->height = max_height;
    if (max_height == 0) {
        // Leaf node: store value.
        size_t index = (key >> 0) % RTREE_ENTS_PER_NODE;
        atomic_store_explicit(&node->data[index], value, memory_order_release);
        node->occupancy = (value != NULL) ? 1 : 0;
    } else {
        // Internal node: create child subtree.
        size_t        index   = (key >> max_height) % RTREE_ENTS_PER_NODE;
        rtree_node_t *child   = rtree_create_subtree(key, value, max_height - RTREE_BITS_PER_NODE);
        node->children[index] = child;
        if (child) {
            child->parent   = node;
            node->occupancy = 1;
        } else {
            free(node);
            return NULL;
        }
    }
    return node;
}

// Implementation of `rtree_set` and `rtree_cmpxchg`.
static errno_ptr_t rtree_xchg(rtree_t *tree, uint64_t key, void *old_value, void *new_value, bool is_cmpxchg) {
    errno_ptr_t res;
    bool        ie = irq_disable();

    spinlock_t *cur_lock = &tree->lock;
    spinlock_take(cur_lock);
    rtree_node_t *cur = tree->root;

    if (new_value == NULL && (cur == NULL || (cur->height < 64 && (key >> cur->height) >= RTREE_ENTS_PER_NODE))) {
        // Setting NULL into an out-of-tree location.
        res.errno = 0;
        res.ptr   = NULL;

    } else if (cur == NULL) {
        // Make new tree for key.
        res.errno = 0;
        res.ptr   = NULL;
        if (!is_cmpxchg || old_value == NULL) {
            uint8_t height     = 64 - __builtin_clzll(key);
            height             = height / RTREE_BITS_PER_NODE * RTREE_BITS_PER_NODE;
            rtree_node_t *root = rtree_create_subtree(key, new_value, height);
            tree->root         = root;
            if (!root) {
                res.errno = -ENOMEM;
            }
        }

    } else {
        while (cur->height < 64 && (key >> cur->height) >= RTREE_ENTS_PER_NODE) {
            // Make tree tall enough to fit the key.
            rtree_node_t *root = calloc(1, sizeof(rtree_node_t));
            if (!root) {
                spinlock_release(cur_lock);
                irq_enable_if(ie);
                res.errno = -ENOMEM;
                return res;
            }
            root->occupancy   = 1;
            root->height      = cur->height + RTREE_BITS_PER_NODE;
            root->children[0] = cur;
            cur->parent       = root;
            tree->root        = root;
            cur               = root;
        }

        while (1) {
            // Walk tree then update.
            size_t index = (key >> cur->height) % RTREE_ENTS_PER_NODE;
            if (cur->height == 0) {
                // Exchange in leaf node.
                bool did_update;
                if (is_cmpxchg) {
                    did_update = atomic_compare_exchange_strong_explicit(
                        &cur->data[index],
                        &old_value,
                        new_value,
                        memory_order_acq_rel,
                        memory_order_acquire
                    );
                    res.ptr = old_value;
                } else {
                    res.ptr    = atomic_exchange_explicit(&cur->data[index], new_value, memory_order_acq_rel);
                    did_update = true;
                }
                if (did_update && res.ptr && !new_value) {
                    cur->occupancy--;
                } else if (did_update && !res.ptr && new_value) {
                    cur->occupancy++;
                }
                break;

            } else {
                spinlock_t *next_lock = &cur->lock;
                spinlock_take(next_lock);
                rtree_node_t *next = cur->children[index];
                if (next) {
                    // Walk further down the tree.
                    spinlock_release(cur_lock);
                    cur      = next;
                    cur_lock = next_lock;

                } else {
                    // Create new leaf node.
                    next                 = rtree_create_subtree(key, new_value, cur->height - RTREE_BITS_PER_NODE);
                    cur->children[index] = next;
                    cur->occupancy++;
                    spinlock_release(next_lock);
                    res.errno = next ? 0 : -ENOMEM;
                    res.ptr   = NULL;
                    break;
                }
            }
        }
    }

    spinlock_release(cur_lock);
    irq_enable_if(ie);

    if (!new_value && (!is_cmpxchg || (res.errno >= 0 && res.ptr == old_value))) {
        rtree_gc_key(tree, key);
    }
    return res;
}

// Insert a value into the radix tree.
errno_ptr_t rtree_set(rtree_t *tree, uint64_t key, void *value) {
    return rtree_xchg(tree, key, NULL, value, false);
}

// Compare-exchange a value in radix tree.
// Returns the old value regardless of success.
errno_ptr_t rtree_cmpxchg(rtree_t *tree, uint64_t key, void *old_value, void *new_value) {
    return rtree_xchg(tree, key, old_value, new_value, true);
}

// Clear the radix tree.
void rtree_clear(rtree_t *tree) {
}



// Get the first entry in the radix tree.
rtree_iter_t rtree_first(rtree_t *tree) {
    TODO();
}

// Get the next entry in the radix tree.
rtree_iter_t rtree_next(rtree_t *tree, rtree_iter_t cur) {
    TODO();
}

static void print_ptr(int indent, void *value) {
    rawprint(" val 0x");
    rawprinthex((size_t)value, sizeof(size_t) * 2);
    rawprint("\n");
}

static void rtree_dump_impl(rtree_node_t *node, void (*printer)(int indent, void *value), int indent) {
    rawprint(": ");
    rawprintdec(node->occupancy, 0);
    rawprint("\n");
    for (size_t i = 0; i < RTREE_ENTS_PER_NODE; i++) {
        if (!node->children[i]) {
            continue;
        }
        for (int x = 0; x < indent; x++) {
            rawprint("  ");
        }
        rawprint("[");
        rawprintdec((int64_t)i, 2);
        rawprint(" @ ");
        rawprintdec(node->height, 0);
        rawprint("]");
        if (node->height == 0) {
            rawprint(":");
            printer(indent + 1, node->data[i]);
        } else {
            rtree_dump_impl(node->children[i], printer, indent + 1);
        }
    }
}

// Debug-print a radix tree.
void rtree_dump(rtree_t *tree, void (*printer)(int indent, void *value)) {
    if (tree->root == NULL) {
        rawprint("<empty>\n");
    } else {
        rawprint("[root]");
        rtree_dump_impl(tree->root, printer ?: print_ptr, 1);
    }
}
