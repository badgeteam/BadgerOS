
// SPDX-License-Identifier: MIT

#pragma once

#include <stddef.h>
#include <stdint.h>



// A simple string with length.
typedef struct lstr lstr_t;



// A simple string with length.
// Intended to be used through a pointer.
struct lstr {
    // Has data pointer?
    size_t is_ptr : 1;
    // Byte length.
    size_t len : __SIZE_WIDTH__ - 1;
    union {
        // Data pointer.
        char *ptr;
        // Data.
        char  data[0];
    };
};



// Create an `lstr_t` from a string literal.
#define LSTR(literal)             ((lstr_t){.is_ptr = 0, .len = sizeof(literal) - 1, .ptr = (literal)})
// Create an `lstr_t` from a length and data pointer.
#define LSTR_FROM_PTR(len_, ptr_) ((lstr_t){.is_ptr = 1, .len = (len_), .ptr = (ptr_)})

// Get the data pointer of an `lstr_t`.
#define lstr_ptr(lstr)                                                                                                 \
    ({                                                                                                                 \
        lstr_t const *lstr_tmp_ = (lstr);                                                                              \
        lstr_tmp_->is_ptr ? lstr_tmp_->ptr : lstr_tmp_->data;                                                          \
    })

// Hashing function for `lstr_t`.
uint32_t lstr_hash(lstr_t const *lstr);
// Cloning function for `lstr_t`.
lstr_t  *lstr_clone(lstr_t const *lstr);
// Compare two `lstr_t` instances.
int      lstr_cmp(lstr_t const *a, lstr_t const *b);
