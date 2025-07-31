
// SPDX-License-Identifier: MIT

#include "lstr.h"

#include "badge_strings.h"
#include "hash.h"

#include <malloc.h>



// Hashing function for `lstr_t`.
uint32_t lstr_hash(lstr_t const *lstr) {
    return hash_bytes(lstr_ptr(lstr), lstr->len);
}

// Cloning function for `lstr_t`.
lstr_t *lstr_clone(lstr_t const *lstr) {
    size_t  size = sizeof(size_t) + lstr->len;
    lstr_t *mem  = malloc(size);
    if (mem) {
        mem_copy(mem, lstr, size);
    }
    return mem;
}

// Compare two `lstr_t` instances.
int lstr_cmp(lstr_t const *a, lstr_t const *b) {
    if (a->len != b->len) {
        return 1;
    }
    return !mem_equals(lstr_ptr(a), lstr_ptr(b), a->len);
}
