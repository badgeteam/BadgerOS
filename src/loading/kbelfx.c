
// SPDX-License-Identifier: MIT

#include "assertions.h"
#include "badge_strings.h"
#include "filesystem.h"
#include "malloc.h"
#include "userland/memory.h"
#include "userland/process.h"

#include <kbelf.h>



// Measure the length of `str`.
size_t kbelfq_strlen(char const *str) {
    return cstr_length(str);
}

// Copy string from `src` to `dst`.
void kbelfq_strcpy(char *dst, char const *src) {
    cstr_copy(dst, SIZE_MAX, src);
}

// Find last occurrance of `c` in `str`.
char const *kbelfq_strrchr(char const *str, char c) {
    ptrdiff_t off = cstr_last_index(str, c);
    return off == -1 ? NULL : off + str;
}

// Compare string `a` to `b`.
bool kbelfq_streq(char const *a, char const *b) {
    return cstr_equals(a, b);
}

// Copy memory from `src` to `dst`.
void kbelfq_memcpy(void *dst, void const *src, size_t nmemb) {
    mem_copy(dst, src, nmemb);
}

// Fill memory `dst` with `c`.
void kbelfq_memset(void *dst, uint8_t c, size_t nmemb) {
    mem_set(dst, c, nmemb);
}

// Compare memory `a` to `b`.
bool kbelfq_memeq(void const *a, void const *b, size_t nmemb) {
    return mem_equals(a, b, nmemb);
}



// Memory allocator function to use for allocating metadata.
// User-defined.
void *kbelfx_malloc(size_t len) {
    return malloc(len);
}
// Memory allocator function to use for allocating metadata.
// User-defined.
void *kbelfx_realloc(void *mem, size_t len) {
    return realloc(mem, len);
}
// Memory allocator function to use for allocating metadata.
// User-defined.
void kbelfx_free(void *mem) {
    free(mem);
}


// Memory allocator function to use for loading program segments.
// Takes a segment with requested address and permissions and returns a segment with physical and virtual address
// information. Returns success status. User-defined.
bool kbelfx_seg_alloc(kbelf_inst inst, size_t segs_len, kbelf_segment *segs) {
    process_t *proc = get_process(kbelf_inst_getpid(inst));
    assert_dev_keep(proc != NULL);
    assert_dev_keep(proc->memmap.segs_base == 0);

    size_t min_addr  = SIZE_MAX;
    size_t max_addr  = 0;
    size_t min_align = 16;

    for (size_t i = 0; i < segs_len; i++) {
        if (segs[i].vaddr_req < min_addr)
            min_addr = segs[i].vaddr_req;
        size_t end = segs[i].vaddr_req + segs[i].size;
        if (end > max_addr)
            max_addr = end;
    }

    size_t vaddr_real = user_map(proc, min_addr, max_addr - min_addr, min_align);
    if (!vaddr_real)
        return false;

    for (size_t i = 0; i < segs_len; i++) {
        segs[i].vaddr_real = segs[i].vaddr_req - min_addr + vaddr_real;
        segs[i].paddr      = segs[i].vaddr_real;
    }

    return true;
}
// Memory allocator function to use for loading program segments.
// Takes a previously allocated segment and unloads it.
// User-defined.
void kbelfx_seg_free(kbelf_inst inst, size_t segs_len, kbelf_segment *segs) {
    (void)segs_len;
    (void)segs;
    process_t *proc = get_process(kbelf_inst_getpid(inst));
    assert_dev_keep(proc != NULL);
    assert_dev_keep(proc->memmap.segs_base != 0);
    user_unmap(proc, proc->memmap.segs_base);
}


// Open a binary file for reading.
// User-defined.
void *kbelfx_open(char const *path) {
    file_t fd = fs_open(NULL, path, 0);
    if (fd == -1)
        return NULL;
    else
        return (void *)(fd + 1);
}

// Close a file.
// User-defined.
void kbelfx_close(void *fd) {
    fs_close(NULL, (int)fd - 1);
}

// Reads a single byte from a file.
// Returns byte on success, -1 on error.
// User-defined.
int kbelfx_getc(void *fd) {
    char      buf;
    fileoff_t len = fs_read(NULL, (int)fd - 1, &buf, 1);
    return len > 0 ? buf : -1;
}

// Reads a number of bytes from a file.
// Returns the number of bytes read, or less than that on error.
// User-defined.
int kbelfx_read(void *fd, void *buf, int buf_len) {
    return fs_read(NULL, (int)fd - 1, buf, buf_len);
}

// Sets the absolute offset in the file.
// Returns 0 on success, -1 on error.
// User-defined.
int kbelfx_seek(void *fd, long pos) {
    fileoff_t q = fs_seek(NULL, (int)fd - 1, pos, SEEK_ABS);
    return pos == q ? 0 : -1;
}


// Find and open a dynamic library file.
// Returns non-null on success, NULL on error.
// User-defined.
kbelf_file kbelfx_find_lib(char const *needed) {
    (void)needed;
    return NULL;
}
