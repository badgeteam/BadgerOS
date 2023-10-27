// This is a header file to allow the compiler to inline these functions without LTO

#pragma once

#include "skiplist.h"

#define LOCK_GROUP_MAX_SIZE SKIPLIST_MAX_HEIGHT * 4

typedef struct {
    skiplist_node_t *nodes[LOCK_GROUP_MAX_SIZE];
    uint8_t locked_count;
    uint8_t total_locked_count;
} skiplist_lock_group_t;

enum lock_group_status {
    FAIL = 0,
    LOCK = 1,
    RELOCK = 2
};

__attribute__((always_inline)) static inline void wait() {
    // sleep twice to help prevent contestion
    intr_pause();
    intr_pause();
}

__attribute__((always_inline)) static inline size_t get_node_index(skiplist_t* list, skiplist_node_t* node) {
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
    BADGEROS_MALLOC_MSG_TRACE("try_lock_node(%zi) %p", get_node_index(list, node), node);
    return !atomic_flag_test_and_set_explicit(&node->modifying, memory_order_acquire);
}

__attribute__((always_inline)) static inline void unlock_node(skiplist_t* list, skiplist_node_t *node, skiplist_lock_group_t* lock_group) {
    BADGEROS_MALLOC_MSG_TRACE("unlock_node(%zi) %p", get_node_index(list, node), node);
    lock_group->total_locked_count -= 1;
    atomic_flag_clear_explicit(&node->modifying, memory_order_release);
}

__attribute__((always_inline)) static inline bool lockgroup_is_locked(skiplist_t* list, skiplist_node_t *node, skiplist_lock_group_t* lock_group) {
    for (int i = 0; i < lock_group->locked_count; i++) {
        if (lock_group->nodes[i] == node) {
            return true;
        }
    }

    return false;
}

__attribute__((always_inline)) static inline void lockgroup_lock_add(skiplist_t* list, skiplist_node_t *node, skiplist_lock_group_t* lock_group) {
    BADGEROS_MALLOC_MSG_TRACE("lockgroup_lock_add(%zi) %p", get_node_index(list, node), node);
    BADGEROS_MALLOC_ASSERT_ERROR(lock_group->locked_count < LOCK_GROUP_MAX_SIZE, "Lock group full");
    BADGEROS_MALLOC_ASSERT_DEBUG(!lockgroup_is_locked(list, node, lock_group), "Node already locked");

    lock_group->nodes[lock_group->locked_count] = node;
    lock_group->locked_count += 1;
}

// This function will try to lock a node, but only if it hasn't been locked before
__attribute__((always_inline)) static inline enum lock_group_status lockgroup_lock(skiplist_t* list, skiplist_node_t *node, skiplist_lock_group_t* lock_group, bool add) {
    BADGEROS_MALLOC_MSG_TRACE("lockgroup_lock(%zi) %p", get_node_index(list, node), node);
    if (lockgroup_is_locked(list, node, lock_group)) {
       BADGEROS_MALLOC_MSG_TRACE("node (%zi) already locked, relock", get_node_index(list, node));
       return RELOCK;  // Node was previously locked by us
    }

    if (try_lock_node(list, node)) {
        lock_group->total_locked_count += 1;
        if (add) {
            lockgroup_lock_add(list, node, lock_group);
            BADGEROS_MALLOC_MSG_TRACE("node (%zi) locked, count: %i, added", get_node_index(list, node), lock_group->locked_count);
        } else {
            BADGEROS_MALLOC_MSG_TRACE("node (%zi) locked, count: %i, not added", get_node_index(list, node), lock_group->locked_count);
        }
        return LOCK;
    }

    BADGEROS_MALLOC_MSG_TRACE("node (%zi) already locked, failed", get_node_index(list, node));
    return FAIL;
}

__attribute__((always_inline)) static inline void lockgroup_unlock(skiplist_t* list, skiplist_lock_group_t* lock_group) {
    BADGEROS_MALLOC_ASSERT_ERROR(lock_group->locked_count == lock_group->total_locked_count, "Lock group leaked %i nodes", lock_group->total_locked_count - lock_group->locked_count);
    for (int j = 0; j < lock_group->locked_count; j++) {
        unlock_node(list, lock_group->nodes[j], lock_group);
    }
    lock_group->locked_count = 0;
}

__attribute__((always_inline)) static inline void skiplist_index_find_prev(skiplist_t *list, size_t index, skiplist_node_t *prev[SKIPLIST_MAX_HEIGHT], skiplist_lock_group_t* lock_group) {
    BADGEROS_MALLOC_MSG_TRACE("skiplist_index_find_prev(%zi)", index);
    uint8_t initial_locked_count = lock_group->locked_count;
    enum lock_group_status lock_status;

start:
    skiplist_node_t *current = &list->head_index;
    if (!(lock_status = lockgroup_lock(list, current, lock_group, false))) {
        BADGEROS_MALLOC_MSG_TRACE("head locked, retrying");
        wait();
        goto start;
    }

    for (int i = SKIPLIST_MAX_HEIGHT - 1; i >= 0; i--) {
        // Traverse to find insertion point
        while (current->next_index[i]) {
            if (current->next_index[i] > (index - 1)) {
                break;
            }

            skiplist_node_t* next = &list->nodes[current->next_index[i]];
            BADGEROS_MALLOC_ASSERT_ERROR(current != next, "loop detected in index skiplist");
	    if (lock_status == LOCK) {
            	unlock_node(list, current, lock_group);
	    }
            current = next;

            if (!(lock_status = lockgroup_lock(list, current, lock_group, false))) {
                BADGEROS_MALLOC_MSG_TRACE("node(%zi) locked, retrying", get_node_index(list, current));

		for (int j = initial_locked_count; j < lock_group->locked_count; j++) {
		    unlock_node(list, lock_group->nodes[j], lock_group);
		}
		lock_group->locked_count = initial_locked_count;
                BADGEROS_MALLOC_ASSERT_ERROR(lock_group->locked_count == lock_group->total_locked_count, "leaked a lock");
                goto start;
            }
        }
        if (lock_status == LOCK) {
            lockgroup_lock_add(list, current, lock_group);
            lock_status = RELOCK; // we now are in fact in the lock group
        }
        prev[i] = current;
    }

    BADGEROS_MALLOC_ASSERT_ERROR(lock_group->locked_count == lock_group->total_locked_count, "leaked a lock");
    BADGEROS_MALLOC_MSG_TRACE("prev at index %i, locked %i nodes", get_node_index(list, prev[0]), lock_group->locked_count - initial_locked_count);
}

__attribute__((always_inline)) static inline void skiplist_size_find_prev(skiplist_t *list, size_t size, skiplist_node_t *prev[SKIPLIST_MAX_HEIGHT], skiplist_lock_group_t* lock_group) {
    BADGEROS_MALLOC_MSG_TRACE("skiplist_size_find_prev(%zi)", size);
    uint8_t initial_locked_count = lock_group->locked_count;
    enum lock_group_status lock_status;
    enum lock_group_status next_lock_status;

start:
    skiplist_node_t *current = &list->head_size;
    if (!(lock_status = lockgroup_lock(list, current, lock_group, false))) {
        BADGEROS_MALLOC_MSG_TRACE("head locked, retrying");
        wait();
        goto start;
    }

    for (int i = SKIPLIST_MAX_HEIGHT - 1; i >= 0; i--) {
        // Traverse to find insertion point
        while (current->next_size[i]) {
            skiplist_node_t* next = &list->nodes[current->next_size[i]];
            BADGEROS_MALLOC_ASSERT_ERROR(current != next, "loop detected in size skiplist");

            if (!(next_lock_status = lockgroup_lock(list, next, lock_group, false))) {
                BADGEROS_MALLOC_MSG_TRACE("node(%zi) locked, retrying", get_node_index(list, next));

		for (int j = initial_locked_count; j < lock_group->locked_count; j++) {
		    unlock_node(list, lock_group->nodes[j], lock_group);
		}
		lock_group->locked_count = initial_locked_count;

                if (lock_status == LOCK) {
                    unlock_node(list, current, lock_group);
                }
                BADGEROS_MALLOC_ASSERT_ERROR(lock_group->locked_count == lock_group->total_locked_count, "leaked a lock");
                goto start;
	    }

	    if (next->size > size) {
		// Current node is the prior node
   	        if (next_lock_status == LOCK) {
            	    unlock_node(list, next, lock_group);
	        }
		break;
	    }

            if (lock_status == LOCK) {
                unlock_node(list, current, lock_group);
            }

            current = next;
	    lock_status = next_lock_status;
        }

        if (lock_status == LOCK) {
            lockgroup_lock_add(list, current, lock_group);
            lock_status = RELOCK;
        }
        prev[i] = current;
    }

    BADGEROS_MALLOC_ASSERT_ERROR(lock_group->locked_count == lock_group->total_locked_count, "leaked a lock");
    BADGEROS_MALLOC_MSG_TRACE("prev at index %i, locked %i nodes", get_node_index(list, prev[0]), lock_group->locked_count - initial_locked_count);
}

__attribute__((always_inline)) static inline int compare_range_size_index(skiplist_t *list, skiplist_node_t* a, skiplist_node_t* b) {
    if (a->size < b->size) return -1;
    if (a->size > b->size) return 1;

    size_t a_index = get_node_index(list, a);
    size_t b_index = get_node_index(list, b);
    if (a_index < b_index) return -1;
    if (a_index > b_index) return 1;

    //printf("compare_range duplicate %p:%p\n", a, b);
    //print_size_skiplist();
    exit(1);
    return 0;  // Both size and index are equal
}

__attribute__((always_inline)) static inline bool skiplist_lock_index_neighbors(skiplist_t* list, skiplist_node_t *node, skiplist_lock_group_t* lock_group) {
    uint8_t node_height = node->height;
    BADGEROS_MALLOC_MSG_TRACE("skiplist_lock_index_neighbors(%zi) height: %i", get_node_index(list, node), node_height);

    for (int i = 0; i < node_height; i++) {
        BADGEROS_MALLOC_MSG_TRACE("locking level %i", i);
        skiplist_node_t* prev_index;

        if (node->prev_index[i]) {
            prev_index = &list->nodes[node->prev_index[i]];
        } else {
            prev_index = &list->head_index; // 0 means head in prev
        }

        if (!lockgroup_lock(list, prev_index, lock_group, true)) {
            BADGEROS_MALLOC_MSG_TRACE("prev_index node(%zi) locked, exiting", get_node_index(list, prev_index));
            return false;
        }

        if (node->next_index[i]) { // next == 0 is no node
            skiplist_node_t* next_index = &list->nodes[node->next_index[i]];

            if (!lockgroup_lock(list, next_index, lock_group, true)) {
                BADGEROS_MALLOC_MSG_TRACE("next node(%zi) locked, exiting", get_node_index(list, next_index));
                return false;
            }
        }
    }

    return true;
}

__attribute__((always_inline)) static inline bool skiplist_lock_size_neighbors(skiplist_t* list, skiplist_node_t *node, skiplist_lock_group_t* lock_group) {
    uint8_t node_height = node->height;
    BADGEROS_MALLOC_MSG_TRACE("skiplist_lock_size_neighbors(%zi) height: %i", get_node_index(list, node), node_height);

    for (int i = 0; i < node_height; i++) {
        BADGEROS_MALLOC_MSG_TRACE("locking level %i", i);
        skiplist_node_t* prev_size;

        if (node->prev_size[i]) {
            prev_size = &list->nodes[node->prev_size[i]];
        } else {
            prev_size = &list->head_size; // 0 means head in prev
        }

        if (!lockgroup_lock(list, prev_size, lock_group, true)) {
            BADGEROS_MALLOC_MSG_TRACE("prev_size node(%zi) locked, exiting", get_node_index(list, prev_size));
            return false;
        }

        if (node->next_size[i]) { // next == 0 is no node
            skiplist_node_t* next_size = &list->nodes[node->next_size[i]];

            if (!lockgroup_lock(list, next_size, lock_group, true)) {
                BADGEROS_MALLOC_MSG_TRACE("next node(%zi) locked, exiting", get_node_index(list, next_size));
                return false;
            }
        }
    }

    return true;
}

__attribute__((always_inline)) static inline void skiplist_remove_index(skiplist_t *list, skiplist_node_t *node, skiplist_lock_group_t* lock_group) {
    uint8_t node_height = node->height;

    for (int i = 0; i < node_height; i++) {
        if (node->next_index[i]) {
            skiplist_node_t* next = &list->nodes[node->next_index[i]];
            BADGEROS_MALLOC_ASSERT_DEBUG(lockgroup_is_locked(list, next, lock_group), "node not locked");
            BADGEROS_MALLOC_MSG_TRACE("level %i, updating next index %i, previous: %i, new: %i", i, node->next_index[i], next->next_index[i], node->prev_index[i]);
            next->prev_index[i] = node->prev_index[i];
        }

        skiplist_node_t* prev;
        if (node->prev_index[i]) {
            prev = &list->nodes[node->prev_index[i]];
        } else {
            prev = &list->head_index; // 0 means head in prev
        }

        BADGEROS_MALLOC_ASSERT_DEBUG(lockgroup_is_locked(list, prev, lock_group), "node not locked");
        BADGEROS_MALLOC_MSG_TRACE("level %i, updating prev index %i, previous: %i, new: %i", i, node->prev_index[i], prev->next_index[i], node->next_index[i]);
        prev->next_index[i] = node->next_index[i];
    }
}

__attribute__((always_inline)) static inline void skiplist_remove_size(skiplist_t *list, skiplist_node_t *node, skiplist_lock_group_t* lock_group) {
    uint8_t node_height = node->height;

    for (int i = 0; i < node_height; i++) {
        if (node->next_size[i]) {
            skiplist_node_t* next = &list->nodes[node->next_size[i]];
            BADGEROS_MALLOC_ASSERT_DEBUG(lockgroup_is_locked(list, next, lock_group), "node not locked");
            BADGEROS_MALLOC_MSG_TRACE("level %i, updating next size %i, previous: %i, new: %i", i, node->next_size[i], next->prev_size[i], node->prev_size[i]);
            next->prev_size[i] = node->prev_size[i];
        }

        skiplist_node_t* prev;
        if (node->prev_size[i]) {
            prev = &list->nodes[node->prev_size[i]];
        } else {
            prev = &list->head_size; // 0 means head in prev
        }

        BADGEROS_MALLOC_ASSERT_DEBUG(lockgroup_is_locked(list, prev, lock_group), "node not locked");
        BADGEROS_MALLOC_MSG_TRACE("level %i, updating prev size %i, previous: %i, new: %i", i, node->prev_size[i], prev->next_size[i], node->next_size[i]);
        prev->next_size[i] = node->next_size[i];
    }
}

__attribute__((always_inline)) static inline bool skiplist_remove(skiplist_t *list, size_t index, skiplist_lock_group_t* lock_group) {
    BADGEROS_MALLOC_MSG_TRACE("skiplist_remove(%zi)", index);

    skiplist_node_t *node = &list->nodes[index];

    if (!lockgroup_lock(list, node, lock_group, true)) {
        BADGEROS_MALLOC_MSG_TRACE("node(%zi) locked, exiting", index);
        return false;
    }

    if (!skiplist_lock_index_neighbors(list, node, lock_group)) {
        BADGEROS_MALLOC_MSG_TRACE("failed to lock index neighbors, exiting");
        return false;
    }

    if (!skiplist_lock_size_neighbors(list, node, lock_group)) {
        BADGEROS_MALLOC_MSG_TRACE("failed to lock size neighbors, exiting");
        return false;
    }

    skiplist_remove_index(list, node, lock_group);
    skiplist_remove_size(list, node, lock_group);

    return true;
}

bool skiplist_insert(skiplist_t *list, size_t index, size_t size) {
    BADGEROS_MALLOC_MSG_TRACE("skiplist_insert(%zi, %zi)", index, size); 
    BADGEROS_MALLOC_ASSERT_ERROR(index <= list->size, "Invalid Free: index %zi > list size %zi", index, list->size);
    index += 1; // index 0 is our free page
    skiplist_lock_group_t lock_group = {0};

start:
    skiplist_node_t *node = &list->nodes[index];
    node->height = determine_node_height(&list->nodes[index]);

    skiplist_node_t *prev[SKIPLIST_MAX_HEIGHT] = {0};
    skiplist_node_t *prev_size[SKIPLIST_MAX_HEIGHT] = {0};
    size_t new_size = size;

    // Find the index insertion point and lock all neighbors
    skiplist_index_find_prev(list, index, prev, &lock_group);
    BADGEROS_MALLOC_ASSERT_DEBUG(lockgroup_is_locked(list, prev[0], &lock_group), "node not locked");
    for (int i = 0; i < node->height; ++i) {
        if (!skiplist_lock_index_neighbors(list, prev[i], &lock_group)) {
            BADGEROS_MALLOC_MSG_TRACE("failed to lock index neigbors, retrying"); 
            goto restart;
        }
    }

    // At this point, all necessary nodes are locked
    if (prev[0]->next_index[0] && prev[0]->next_index[0] < (index + new_size)) {
        BADGEROS_MALLOC_MSG_ERROR("Invalid Free: %zi of size %zi overlaps next index: %i", index - 1, new_size, prev[0]->next_index[0] - 1);
        goto out;
    }

    if (get_node_index(list, prev[0]) + prev[0]->size > index) {
        BADGEROS_MALLOC_MSG_ERROR("Invalid Free: %zi of size %zi overlaps prev index: %zi of size %i", index - 1, new_size, get_node_index(list, prev[0]), prev[0]->size);
        goto out;
    }
    
    // Check to see if we need to coalesce into the next node
    if (prev[0]->next_index[0] == (index + size)) {
        skiplist_node_t* next = &list->nodes[prev[0]->next_index[0]];

        BADGEROS_MALLOC_MSG_ERROR("coalescing into next node"); 
        if(!skiplist_remove(list, prev[0]->next_index[0], &lock_group)) {
            BADGEROS_MALLOC_MSG_TRACE("coalescing failed: can't remove next node, retrying"); 
            goto restart;
        }

        new_size += next->size;
    }

    // Check to see if we need to coalesce into the previous node
    if (prev[0] != &list->head_index) {
        if (get_node_index(list, prev[0]) + prev[0]->size == index) {
            BADGEROS_MALLOC_MSG_TRACE("coalescing into previous node"); 
#if 0

            if (!skiplist_lock_size_neighbors(list, prev[0], &lock_group)) {
                BADGEROS_MALLOC_MSG_ERROR("failed to lock size neighbors, retrying");
                goto restart;
            }

            skiplist_remove_size(list, prev[0], &lock_group);
            skiplist_size_find_prev(list, prev[0]->size + new_size, prev_size, &lock_group);
            if (!skiplist_lock_size_neighbors(list, prev_size[0], &lock_group)) {
                BADGEROS_MALLOC_MSG_ERROR("failed to lock new size neigbors, retrying"); 
                skiplist_reinsert_size(list, prev[0], &lock_group);
                goto restart;
            }

            prev[0]->size += new_size;
            node = prev[0];
            goto out_size;
            //prev[0]->size += new_size;
            //goto out;
#endif
            if (skiplist_remove(list, get_node_index(list, prev[0]), &lock_group)) {
                size = prev[0]->size + new_size;
                BADGEROS_MALLOC_MSG_DEBUG("re-inserting previous node (%i) %p with new size %zi instead of %i", get_node_index(list, prev[0]), prev[0], size, index);
                index = get_node_index(list, prev[0]);
                goto start;
            } else {
                BADGEROS_MALLOC_MSG_TRACE("failed to remove previous node");
                goto restart;
            }

            //prev[0]->size += new_size;
            //goto out;
        }
    }

    node->size = new_size;

    skiplist_size_find_prev(list, node->size, prev_size, &lock_group);
    for (int i = 0; i < node->height; ++i) {
        if (!skiplist_lock_size_neighbors(list, prev_size[i], &lock_group)) {
            BADGEROS_MALLOC_MSG_TRACE("failed to lock size neigbors, retrying"); 
            goto restart;
        }
    }

    // Perform the index insertion
    BADGEROS_MALLOC_MSG_TRACE("index: inserting node(%i) %p at height %i", index, node, node->height); 
    for (int i = 0; i < node->height; i++) {
        if (prev[i]->next_index[i]) {
            skiplist_node_t* next = &list->nodes[prev[i]->next_index[i]];
            BADGEROS_MALLOC_ASSERT_ERROR(next != node, "creating loop in index skiplist (next == node)");
            BADGEROS_MALLOC_ASSERT_DEBUG(lockgroup_is_locked(list, next, &lock_group), "node(%i) %p not locked at height %i", get_node_index(list, next), next, i);
            next->prev_index[i] = index;
        }

        BADGEROS_MALLOC_ASSERT_ERROR(prev[i] != node, "creating loop in index skiplist (prev[%i] == node)", i);
        node->next_index[i] = prev[i]->next_index[i];
        node->prev_index[i] = get_node_index(list, prev[i]);
        BADGEROS_MALLOC_ASSERT_DEBUG(lockgroup_is_locked(list, prev[i], &lock_group), "node not locked");
        prev[i]->next_index[i] = index;
    }

    // Perform the size insertion
    BADGEROS_MALLOC_MSG_TRACE("size: inserting node(%i) %p at height %i", index, node, node->height); 
    for (int i = 0; i < node->height; i++) {
        if (prev_size[i]->next_size[i]) {
            skiplist_node_t* next = &list->nodes[prev_size[i]->next_size[i]];
            BADGEROS_MALLOC_ASSERT_ERROR(next != node, "creating loop in size skiplist (next == node)");
            BADGEROS_MALLOC_ASSERT_DEBUG(lockgroup_is_locked(list, next, &lock_group), "node(%i) %p not locked at height %i", get_node_index(list, next), next, i);
            next->prev_size[i] = get_node_index(list, node);
        }

        BADGEROS_MALLOC_ASSERT_ERROR(prev_size[i] != node, "creating loop in size skiplist (prev_size[%i] == node)", i);
        node->next_size[i] = prev_size[i]->next_size[i];
        node->prev_size[i] = get_node_index(list, prev_size[i]);
        BADGEROS_MALLOC_ASSERT_DEBUG(lockgroup_is_locked(list, prev_size[i], &lock_group), "node not locked");
        prev_size[i]->next_size[i] = get_node_index(list, node);
    }

out:
    lockgroup_unlock(list, &lock_group);
    return true;

restart:
    lockgroup_unlock(list, &lock_group);
    wait();
    goto start;
}

#ifdef BADGEROS_MALLOC_STANDALONE
void print_skiplist(skiplist_t* list) {
    printf("Index skiplist:\n");
    size_t count = 0;
    for (int i = SKIPLIST_MAX_HEIGHT - 1; i >= 0; i--) {
        skiplist_node_t *current = &list->head_index;
        printf("Level %i: ", i);
        while (current != &list->nodes[0]) {
            int32_t current_index = get_node_index(list, current);
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
    printf("%zi nodes\n", count);

    count = 0;
    printf("Size skiplist:\n");
    for (int i = SKIPLIST_MAX_HEIGHT - 1; i >= 0; i--) {
        skiplist_node_t *current = &list->head_size;
        printf("Level %i: ", i);
        while (current != &list->nodes[0]) {
            uint16_t current_index = get_node_index(list, current);
            printf("%i<(%i)>%i (%i-%i:%i) -> ", current->prev_size[i], current_index, current->next_size[i], current_index, current_index + current->size, current->size);
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
    printf("%zi nodes\n", count);

    if (atomic_flag_test_and_set(&list->head_index)) {
        printf("Index head is locked\n");
    } else {
        atomic_flag_clear(&list->head_index);
    }

    if (atomic_flag_test_and_set(&list->head_size)) {
        printf("Size head is locked\n");
    } else {
        atomic_flag_clear(&list->head_size);
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
