#pragma once

#define ALIGN_UP(x, y)   (void *)(((size_t)(x) + (y - 1)) & ~(y - 1))
#define ALIGN_DOWN(x, y) (void *)((size_t)(x) & ~(y - 1))

#define ALIGN_PAGE_UP(x)   ALIGN_UP(x, PAGE_SIZE)
#define ALIGN_PAGE_DOWN(x) ALIGN_DOWN(x, PAGE_SIZE)


