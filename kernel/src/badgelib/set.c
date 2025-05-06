
// SPDX-FileCopyrightText: 2025 Julian Scheffers <julian@scheffers.net>
// SPDX-FileType: SOURCE
// SPDX-License-Identifier: MIT

#include "set.h"

#include "list.h"
#include "log.h"
#include "malloc.h"
#include "panic.h"



// Remove all entries from a set.
void set_clear(set_t *set) {
    for (size_t i = 0; i < SET_BUCKETS; i++) {
        dlist_node_t *node = dlist_pop_front(&set->buckets[i]);
        while (node) {
            set_ent_t *ent = (set_ent_t *)node;
            set->val_del(ent->value);
            free(ent);
            node = dlist_pop_front(&set->buckets[i]);
        }
    }
    set->len = 0;
}

// Get an item from the set.
bool set_contains(set_t const *set, void const *value) {
    // Figure out which bucket the value is in.
    uint32_t hash   = set->val_hash(value);
    size_t   bucket = hash % SET_BUCKETS;

    // Walk the list of items in this bucket.
    dlist_node_t *node = set->buckets[bucket].head;
    while (node) {
        set_ent_t *ent = (set_ent_t *)node;
        // Both hash and string compare must be equal.
        if (ent->hash == hash && !set->val_cmp(ent->value, value)) {
            return true;
        }
        // Go to the next item in the bucket.
        node = node->next;
    }

    // The bucket did not contain the value.
    return false;
}

// Insert an item into the set.
bool set_add(set_t *set, void const *value) {
    // Figure out which bucket the value is in.
    uint32_t hash   = set->val_hash(value);
    size_t   bucket = hash % SET_BUCKETS;

    // Walk the list of items in this bucket.
    dlist_node_t *node = set->buckets[bucket].head;
    while (node) {
        set_ent_t *ent = (set_ent_t *)node;
        // Both hash and string compare must be equal.
        if (ent->hash == hash && !set->val_cmp(ent->value, value)) {
            // There is an existing value.
            return false;
        }
        node = node->next;
    }

    // Allocate a new item.
    set_ent_t *ent = malloc(sizeof(set_ent_t));
    if (!ent) {
        return false;
    }
    ent->node  = DLIST_NODE_EMPTY;
    ent->value = set->val_dup(value);
    ent->hash  = hash;

    // Add the new item to the bucket.
    dlist_append(&set->buckets[bucket], &ent->node);
    set->len++;
    return true;
}

// Add all items from another set to this one.
size_t set_addall(set_t *set, set_t const *other) {
    if (other->val_hash != set->val_hash || other->val_cmp != set->val_cmp || other->val_dup != set->val_dup ||
        other->val_del != set->val_del) {
        logk(LOG_FATAL, "Error: Sets contain different types\n");
        panic_abort();
    }
    size_t added = 0;

    for (size_t i = 0; i < SET_BUCKETS; i++) {
        dlist_t tmp = DLIST_EMPTY;
        dlist_foreach_node(set_ent_t const, other_ent, &other->buckets[i]) {
            // If the entry exists in this set already, do not add it.
            dlist_foreach_node(set_ent_t const, this_ent, &set->buckets[i]) {
                if (other_ent->hash == this_ent->hash && !set->val_cmp(other_ent->value, this_ent->value)) {
                    goto skip;
                }
            }

            // Allocate a new item.
            set_ent_t *new_ent = malloc(sizeof(set_ent_t));
            if (new_ent) {
                new_ent->node  = DLIST_NODE_EMPTY;
                new_ent->value = set->val_dup(other_ent->value);
                new_ent->hash  = other_ent->hash;
                dlist_append(&tmp, &new_ent->node);
                added++;
                set->len++;
            }

        skip:;
        }

        dlist_concat(&set->buckets[i], &tmp);
    }

    return added;
}

// Retain all items also in the other set.
// Returns how many items, if any, are removed.
size_t set_intersect(set_t *set, set_t const *other) {
    if (other->val_hash != set->val_hash || other->val_cmp != set->val_cmp || other->val_dup != set->val_dup ||
        other->val_del != set->val_del) {
        logk(LOG_FATAL, "Error: Sets contain different types\n");
        panic_abort();
    }
    size_t removed = 0;

    for (size_t i = 0; i < SET_BUCKETS; i++) {
        set_ent_t *ent = (void *)set->buckets[i].head;
        while (ent) {
            bool keep = false;
            dlist_foreach_node(set_ent_t, other_ent, &other->buckets[i]) {
                if (ent->hash == other_ent->hash && set->val_cmp(ent->value, other_ent->value) == 0) {
                    keep = true;
                    break;
                }
            }
            set_ent_t *next = (void *)ent->node.next;
            if (!keep) {
                dlist_remove(&set->buckets[i], &ent->node);
                set->val_del(ent->value);
                free(ent);
                removed++;
            }
            ent = next;
        }
    }

    return removed;
}

// Retain all items that are in the array.
// Returns how many items, if any, are removed.
size_t set_intersect_array(set_t *set, void const *const *values, size_t values_len) {
    size_t removed = 0;

    for (size_t i = 0; i < SET_BUCKETS; i++) {
        set_ent_t *ent = (void *)set->buckets[i].head;
        while (ent) {
            bool keep = false;

            for (size_t j = 0; j < values_len; j++) {
                if (set->val_cmp(ent->value, values[j]) == 0) {
                    keep = true;
                    break;
                }
            }

            set_ent_t *next = (void *)ent->node.next;
            if (!keep) {
                dlist_remove(&set->buckets[i], &ent->node);
                set->val_del(ent->value);
                free(ent);
                removed++;
            }
            ent = next;
        }
    }

    return removed;
}

// Remove an item from the set.
bool set_remove(set_t *set, void const *value) {
    // Figure out which bucket the value is in.
    uint32_t hash   = set->val_hash(value);
    size_t   bucket = hash % SET_BUCKETS;

    // Walk the list of items in this bucket.
    dlist_node_t *node = set->buckets[bucket].head;
    while (node) {
        set_ent_t *ent = (set_ent_t *)node;
        // Both hash and string compare must be equal.
        if (ent->hash == hash && !set->val_cmp(ent->value, value)) {
            // There is an existing value; remove it.
            dlist_remove(&set->buckets[bucket], node);
            set->val_del(ent->value);
            free(ent);
            set->len--;
            return true;
        }
        node = node->next;
    }

    return false;
}

// Get next item in the set (or first if `ent` is NULL).
set_ent_t const *set_next(set_t const *set, set_ent_t const *ent) {
    size_t bucket;
    if (!ent) {
        bucket = 0;
    } else if (ent->node.next) {
        return (set_ent_t const *)ent->node.next;
    } else {
        bucket = ent->hash % SET_BUCKETS + 1;
    }
    while (bucket < SET_BUCKETS) {
        if (set->buckets[bucket].head) {
            return (set_ent_t const *)set->buckets[bucket].head;
        }
        bucket++;
    }
    return NULL;
}



#ifndef NDEBUG
// Print the pointer of all items in a set.
void set_dump(set_t const *set) {
    logkf(
        LOG_DEBUG,
        "Set %{size;x} has %{size;d} item%{cs}\n",
        set,
        set->len,
        set->len == 0   ? "s."
        : set->len == 1 ? ":"
                        : "s:"
    );
    set_foreach(void, item, set) {
        logkf(LOG_DEBUG, "    %{size;x}\n", item);
    }
}
#endif
