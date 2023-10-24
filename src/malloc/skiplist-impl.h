#pragma once

#include "skiplist.h"

__attribute__((always_inline)) static inline void wait() {
    // sleep twice to help prevent contestion
    intr_pause();
    intr_pause();
}

__attribute__((always_inline)) static inline size_t get_page_index(skiplist_t* list, skiplist_node_t* node) {
    if (node == &list->head_index) return 0; // For prev_index 0 indicates the head, for next it indicates nothing
    if (node == &list->head_size) return 0; // For prev_size 0 indicates the head, for next it indicates nothing

    return (size_t)((size_t)node - (size_t)&list->nodes[0]) / sizeof(skiplist_node_t);
}

__attribute__((always_inline)) static inline uint32_t hash_32bit(uintptr_t x) {
    x = ((x >> 16) ^ x) * 0x45d9f3b;
    x = ((x >> 16) ^ x) * 0x45d9f3b;
    x = (x >> 16) ^ x;
    return x;
}

__attribute__((always_inline)) static inline uint64_t hash_64bit(uintptr_t x) {
    x = (x ^ (x >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    x = (x ^ (x >> 27)) * UINT64_C(0x94d049bb133111eb);
    x = x ^ (x >> 31);
    return x;
}

__attribute__((always_inline)) static inline uintptr_t hash_pointer(void* ptr) {
    if (sizeof(void*) == 4)
        return ((uintptr_t)hash_32bit((uintptr_t)ptr));

    return ((uintptr_t)hash_64bit((uintptr_t)ptr));
}

__attribute__((always_inline)) static inline uint8_t determine_node_height(void *ptr) {
    uintptr_t hash = hash_pointer(ptr);
    int level = 1;

    while ((hash & 1) == 1 && level < SKIPLIST_MAX_HEIGHT) {
        level++;
        hash >>= 1;
    }

    return level;
}

__attribute__((always_inline)) static inline bool try_lock_node(skiplist_t* list, skiplist_node_t *node) {
    BADGEROS_MALLOC_MSG_TRACE("try_lock_node(%zi)", get_page_index(list, node));
    return !atomic_flag_test_and_set_explicit(&node->modifying, memory_order_acquire);
}

__attribute__((always_inline)) static inline void unlock_node(skiplist_t* list, skiplist_node_t *node) {
    BADGEROS_MALLOC_MSG_TRACE("unlock_node(%zi)", get_page_index(list, node));
    atomic_flag_clear_explicit(&node->modifying, memory_order_release);
}

// This function will try to lock a node, but only if it hasn't been locked before
__attribute__((always_inline)) static inline bool try_lock_node_unique(skiplist_t* list, skiplist_node_t *node, skiplist_lock_group_t* lock_group) {
    BADGEROS_MALLOC_MSG_TRACE("try_lock_node_unique(%zi)", get_page_index(list, node));
    for (int i = 0; i < lock_group->locked_count; i++) {
        if (lock_group->nodes[i] == node) {
            return true;  // Node was previously locked by us
        }
    }

    if (try_lock_node(list, node)) {
        lock_group->nodes[lock_group->locked_count] = node;  // Add node to our list of locked nodes
        lock_group->locked_count += 1;
        BADGEROS_MALLOC_MSG_TRACE("try_lock_node_unique(%zi) locked, count: %i", get_page_index(list, node), lock_group->locked_count);
        return true;
    }

    BADGEROS_MALLOC_MSG_TRACE("try_lock_node_unique(%zi) failed", get_page_index(list, node));
    return false;
}

__attribute__((always_inline)) static inline void skiplist_unlock(skiplist_t* list, skiplist_lock_group_t* lock_group) {
     for (int j = 0; j < lock_group->locked_count; j++) {
         unlock_node(list, lock_group->nodes[j]);
     }
     lock_group->locked_count = 0;
}

__attribute__((always_inline)) static inline void skiplist_index_find_prev(skiplist_t *list, size_t index, skiplist_node_t *prev[SKIPLIST_MAX_HEIGHT], skiplist_lock_group_t* lock_group) {
    BADGEROS_MALLOC_MSG_TRACE("skiplist_index_find_prev(%zi)", index);
start:
    skiplist_node_t *current = &list->head_index;
    if (!try_lock_node(list, current)) {
        BADGEROS_MALLOC_MSG_TRACE("head locked, retrying");
        wait();
        goto start;
    }

    lock_group->locked_count = 1;
    lock_group->nodes[0] = current;

    for (int i = SKIPLIST_MAX_HEIGHT - 1; i >= 0; i--) {
        // Traverse to find insertion point
        while (current->next_index[i]) {
            if (current->next_index[i] > (index - 1)) {
                break;
            }

            skiplist_node_t* next = &list->nodes[current->next_index[i]];
            unlock_node(list, current);
            current = next;

            if (!try_lock_node(list, current)) {
                BADGEROS_MALLOC_MSG_TRACE("node(%zi) locked, retrying", get_page_index(list, current));
                skiplist_unlock(list, lock_group);
                goto start;
            }
        }
        prev[i] = current;
        if (lock_group->nodes[lock_group->locked_count - 1] != current) {
            lock_group->nodes[lock_group->locked_count] = current;
            lock_group->locked_count += 1;
        }
    }

    BADGEROS_MALLOC_MSG_TRACE("locked %i nodes", lock_group->locked_count);
}

__attribute__((always_inline)) static inline bool skiplist_lock_node_neighbors(skiplist_t* list, skiplist_node_t *node, skiplist_lock_group_t* lock_group) {
    BADGEROS_MALLOC_MSG_TRACE("skiplist_lock_node_neighbours(%zi)", get_page_index(list, node));
    uint8_t node_height = node->height;

    for (int i = 0; i < node_height; i++) {
        skiplist_node_t* prev;

        if (node->prev_index[i]) {
            prev = &list->nodes[node->prev_index[i]];
        } else {
            prev = &list->head_index; // 0 means head in prev
        }

        if (!try_lock_node_unique(list, prev, lock_group)) {
            BADGEROS_MALLOC_MSG_TRACE("prev node(%zi) locked, exiting", get_page_index(list, prev));
            skiplist_unlock(list, lock_group);
            return false; // Couldn't lock one of the prev nodes, unlock all
        }

        if (node->next_index[i]) { // next == 0 is no node
            skiplist_node_t* next = &list->nodes[node->next_index[i]];

            if (!try_lock_node_unique(list, next, lock_group)) {
                BADGEROS_MALLOC_MSG_TRACE("next node(%zi) locked, exiting", get_page_index(list, next));
                skiplist_unlock(list, lock_group);
                return false; // Couldn't lock one of the next nodes, unlock all
            }
        }
    }

    return true;
}

static inline bool skiplist_remove(skiplist_t *list, size_t index, skiplist_lock_group_t* lock_group) {
    BADGEROS_MALLOC_MSG_TRACE("skiplist_remove(%zi)", index);

    skiplist_node_t *node = &list->nodes[index];
    uint8_t node_height = node->height;

    if (!try_lock_node_unique(list, node, lock_group)) {
        BADGEROS_MALLOC_MSG_TRACE("node(%zi) locked, exiting", index);
        return false;
    }

    // Lock all of our neighbors
    if (!skiplist_lock_node_neighbors(list, node, lock_group)) {
        BADGEROS_MALLOC_MSG_TRACE("failed to lock neighbors, exiting");
        return false;
    }

    for (int i = 0; i < node_height; i++) {
        if (node->next_index[i]) {
            skiplist_node_t* next = &list->nodes[node->next_index[i]];
            BADGEROS_MALLOC_MSG_TRACE("level %i, updating next %i, previous: %i, new: %i", i, node->next_index[i], next->next_index[i], node->prev_index[i]);
            next->prev_index[i] = node->prev_index[i];
        }

        skiplist_node_t* prev;
        if (node->prev_index[i]) {
            prev = &list->nodes[node->prev_index[i]];
        } else {
            prev = &list->head_index; // 0 means head in prev
        }

        BADGEROS_MALLOC_MSG_TRACE("level %i, updating prev %i, previous: %i, new: %i", i, node->prev_index[i], prev->next_index[i], node->next_index[i]);
        prev->next_index[i] = node->next_index[i];
    }

    return true;
}

bool skiplist_insert(skiplist_t *list, size_t index, size_t size) {
    BADGEROS_MALLOC_MSG_TRACE("skiplist_insert(%zi, %zi)", index, size); 
    index += 1; // index 0 is our free page
start:
    skiplist_node_t *prev[SKIPLIST_MAX_HEIGHT] = {0};
    skiplist_lock_group_t lock_group = {0};

    int node_height = determine_node_height(&list->nodes[index]);

    skiplist_index_find_prev(list, index, prev, &lock_group);

    if (!skiplist_lock_node_neighbors(list, prev[0], &lock_group)) {
        BADGEROS_MALLOC_MSG_TRACE("failed to lock neigbors, retrying"); 
        skiplist_unlock(list, &lock_group);
        wait();
        goto start;
    }

    // At this point, all necessary nodes are locked
    // Check to see if we need to coalesce into the next node

    if (prev[0]->next_index[0] && prev[0]->next_index[0] < (index + size)) {
        BADGEROS_MALLOC_MSG_ERROR("Invalid Free: %zi of size %zi overlaps next index: %i\n", index - 1, size, prev[0]->next_index[0] - 1);
        goto out;
    }
    if (get_page_index(list, prev[0]) + prev[0]->size > index) {
        BADGEROS_MALLOC_MSG_ERROR("Invalid Free: %zi of size %zi overlaps prev index: %zi of size %i\n", index - 1, size, get_page_index(list, prev[0]), prev[0]->size);
        goto out;
    }
    
    if (prev[0]->next_index[0] == (index + size)) {
        skiplist_node_t* next = &list->nodes[prev[0]->next_index[0]];

        BADGEROS_MALLOC_MSG_TRACE("coalescing into next node"); 
        if(!skiplist_remove(list, prev[0]->next_index[0], &lock_group)) {
            BADGEROS_MALLOC_MSG_TRACE("coalescing failed: can't remove next node, retrying"); 
            goto start;
        }

        size += next->size;
    }

    // Check to see if we need to coalesce into the previous node
    if (prev[0] != &list->head_index) {
        if (get_page_index(list, prev[0]) + prev[0]->size == index) {
            BADGEROS_MALLOC_MSG_TRACE("coalescing into previous node"); 
            prev[0]->size += size;
            goto out;
        }

    }

    skiplist_node_t *node = &list->nodes[index];
    node->size = size;
    node->height = node_height;

    // Perform the insertion
    for (int i = 0; i < node_height; i++) {
        if (prev[i]->next_index[i]) {
            skiplist_node_t* next = &list->nodes[prev[i]->next_index[i]];
            next->prev_index[i] = index;
        }

        node->next_index[i] = prev[i]->next_index[i];
        node->prev_index[i] = get_page_index(list, prev[i]);
        prev[i]->next_index[i] = index;
    }

out:
    skiplist_unlock(list, &lock_group);
    return true;
}

#ifdef BADGEROS_MALLOC_STANDALONE
void print_skiplist(skiplist_t* list) {
    printf("Index skiplist:\n");
    for (int i = SKIPLIST_MAX_HEIGHT - 1; i >= 0; i--) {
        skiplist_node_t *current = &list->head_index;
        printf("Level %i: ", i);
        size_t count = 0;
        while (current != &list->nodes[0]) {
            int32_t current_index = get_page_index(list, current);
            printf("%i<(%i)>%i (%i-%i:%i) -> ", current->prev_index[i], current_index, current->next_index[i], current_index, current_index + current->size, current->size);
            if (current_index && current_index == current->next_index[i]) {
                printf("duplicate node %i\n", current_index);
                exit(1);
            }
            current = &list->nodes[current->next_index[i]];
            if (count >= list->size) {
                printf("LOOP!\n");
                exit(1);
            }
            ++count;
        }
       printf("\n");
    }

    printf("Size skiplist:\n");
    for (int i = SKIPLIST_MAX_HEIGHT - 1; i >= 0; i--) {
        skiplist_node_t *current = &list->head_size;
        printf("Level %i: ", i);
        size_t count = 0;
        while (current != &list->nodes[0]) {
            uint16_t current_index = get_page_index(list, current);
            printf("(%i)>%i(%i-%i:%i) -> ", current_index, current->next_size[i], current_index, current_index + current->size, current->size);
            if (current_index && current_index == current->next_size[i]) {
                printf("duplicate node %i\n", current_index);
                exit(1);
            }
            current = &list->nodes[current->next_size[i]];
            if (count >= list->size) {
                printf("LOOP!\n");
                exit(1);
            }
            ++count;
        }
        printf("\n");
    }

    for (size_t i = 0; i < list->size; ++i) {
        if (atomic_flag_test_and_set(&list->nodes[i].modifying)) {
            printf("Node %zi is locked\n", i);
        } else {
            atomic_flag_clear(&list->nodes[i].modifying);
        }
    }
}
#endif
