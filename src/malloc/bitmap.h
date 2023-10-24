#include <stddef.h>
#include <stdbool.h>
#include <stdatomic.h>

// Each level's bitmap.
typedef struct {
    char* bits; // An array of atomic flags.
    size_t num_bits;   // Total number of bits in this level's bitmap.
    size_t level_size;
} BitmapLevel;

typedef struct {
    BitmapLevel* levels;  // An array of BitmapLevel structs.
    size_t num_levels;    // The number of levels.
    size_t page_size;     // The base size of a page (e.g., 4096 bytes).
} BitmapAllocator;

// Function to compute the number of bits needed for a level.
size_t bits_for_level(size_t pages, size_t level) {
    size_t block_size = (1 << level) * 16;  // Size in pages.
    return (pages - block_size + 1);
}

unsigned int log2_ceil(unsigned int value) {
    if (value == 0) {
        return 0; // or assert or handle error as log2(0) is undefined.
    }

    unsigned int result = 0;
    value--;  // Decrement to handle power-of-2 values correctly.

    while (value >>= 1) {
        result++;
    }

    return result + 1;
}

void print_bitmap(BitmapAllocator* bitmap) {
    for (size_t i = 0; i < bitmap->num_levels; ++i) {
        printf("Level %i (bits: %zi, size: %zi): ", i, bitmap->levels[i].num_bits, bitmap->levels[i].level_size);
        for (size_t k = 0; k < bitmap->levels[i].num_bits; ++k) {
            printf("%i ", bitmap->levels[i].bits[k]);
        }
        printf("\n");
    }
}

const size_t lower_levels[] = { 4, 8, 12, 16 };

size_t init_bitmap(size_t total_pages, void* mem) {
    BitmapAllocator* bitmap = (BitmapAllocator*)mem;

    size_t num_lower_levels = sizeof(lower_levels) / sizeof(lower_levels[0]);

    // Calculate the number of levels.
    size_t level_size = lower_levels[num_lower_levels - 1];
    size_t num_levels = num_lower_levels;
    while (level_size < total_pages) {
        level_size *= 2;
        num_levels++;
    }
    bitmap->num_levels = num_levels;

    // Calculate the memory offset, starting right after the BitmapAllocator structure.
    size_t mem_offset = sizeof(BitmapAllocator);

    // Initialize the lower levels using the defined sizes.
    for (size_t i = 0; i < num_lower_levels; i++) {
        bitmap->levels[i].level_size = lower_levels[i];
        bitmap->levels[i].num_bits = total_pages - lower_levels[i] + 1;
        bitmap->levels[i].bits = (atomic_flag*)((uint8_t*)mem + mem_offset);
        mem_offset += bitmap->levels[i].num_bits * sizeof(atomic_flag);
    }

    // Initialize the levels that double in size.
    level_size = lower_levels[num_lower_levels - 1];
    for (size_t i = num_lower_levels; i < num_levels; i++) {
        level_size *= 2;
        bitmap->levels[i].level_size = level_size;
        bitmap->levels[i].num_bits = total_pages - level_size + 1;
        bitmap->levels[i].bits = (atomic_flag*)((uint8_t*)mem + mem_offset);
        mem_offset += bitmap->levels[i].num_bits * sizeof(atomic_flag);
    }

    // Initialize all atomic flags to clear state.
    for (size_t i = 0; i < num_levels; i++) {
        for (size_t j = 0; j < bitmap->levels[i].num_bits; j++) {
            bitmap->levels[i].bits[j] = 0;
        }
    }

    return mem_offset;  // Return the total memory used (including the BitmapAllocator structure).
}

void mark_bits(BitmapAllocator* bitmap, size_t start, size_t length) {
    printf("mark_bits start: %zi, length: %zi\n", start, length);
    size_t end = start + length;
    size_t level_0_size = bitmap->levels[0].level_size;

    // First, handle the level 0 bitmap.
    size_t start_in_level0 = start / level_0_size;
    size_t end_in_level0 = (start + length + level_0_size - 1) / level_0_size; // ceiling division

    for (size_t i = start_in_level0; i < end_in_level0; ++i) {
        if (i < bitmap->levels[0].num_bits) { // Check for boundary.
            bitmap->levels[0].bits[i] = 1;
        }
    }

    // Now, for each higher level, mark starting positions that overlap with the allocated range.
    for (size_t level = 1; level < bitmap->num_levels; ++level) {
        BitmapLevel* current_level = &bitmap->levels[level];

        for (size_t pos = 0; pos < current_level->num_bits; ++pos) {
            size_t block_start = pos * level_0_size; // Position times base size gives starting page.
            size_t block_end = block_start + current_level->level_size;

            if (block_start < end && block_end > start) {
                // This range for the current level overlaps with the allocated pages.
                current_level->bits[pos] = 1;
            }
        }
    }
}

bool alloc_bitmap(BitmapAllocator* bitmap, size_t size) {
    // Identify the appropriate level for this allocation size.
    size_t level_index = 0;
    while (level_index < bitmap->num_levels && bitmap->levels[level_index].level_size < size) {
        level_index++;
    }

    if (level_index == bitmap->num_levels) {
        return false;  // No level can satisfy this request.
    }

    // Search for a free block in the identified level.
    BitmapLevel* target_level = &bitmap->levels[level_index];
    size_t base_size = bitmap->levels[0].level_size;
    for (size_t i = 0; i < target_level->num_bits; ++i) {
        if (target_level->bits[i] == 0) {
            // Found a free block. Mark it and the corresponding lower blocks as occupied.
            size_t start_position = i * base_size; // This interprets the bit position to the actual starting page number.
            mark_bits(bitmap, start_position, size);
            return true;
        }
    }
    return false;
}

void free_bitmap(BitmapAllocator* bitmap, void* ptr, size_t pages) {
    size_t index = ((char*)ptr - (char*)bitmap) / (bitmap->page_size * 16);
    size_t level = log2_ceil(pages / 16);

    // Clear this and upper levels.
    for(size_t l = level; l < bitmap->num_levels; l++) {
        for(size_t j = index; j < index + (pages / 16); j++) {
            bitmap->levels[l].bits[j] = 0;
        }
    }
}

