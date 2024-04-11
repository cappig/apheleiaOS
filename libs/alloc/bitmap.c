#include "bitmap.h"

#include <base/addr.h>
#include <base/macros.h>
#include <base/types.h>
#include <data/bitmap.h>
#include <string.h>
#include <x86/e820.h>


static inline usize _to_block(bitmap_alloc* alloc, void* ptr) {
    return ((u64)ptr - (uptr)alloc->chuck_start) / alloc->block_size;
}

static inline bitmap_word* _to_ptr(bitmap_alloc* alloc, usize block) {
    return (void*)((uptr)alloc->chuck_start + block * alloc->block_size);
}


bool bitmap_alloc_init(bitmap_alloc* alloc, e820_map* mmap, usize block_size) {
    u64 mem_base = (u64)-1;
    u64 mem_top = 0;

    for (usize i = 0; i < mmap->count; i++) {
        e820_entry* current = &mmap->entries[i];

        if (current->type != E820_AVAILABLE)
            continue;

        u64 top = current->address + current->size;
        u64 base = current->address;

        if (mem_base > base)
            mem_base = base;

        if (mem_top < top)
            mem_top = top;
    }

    u64 mem_size = mem_top - mem_base;

    alloc->chuck_start = (void*)mem_base;
    alloc->chunk_size = mem_size;

    alloc->block_size = block_size;
    alloc->block_count = mem_size / block_size;
    alloc->word_count = alloc->block_count / BITMAP_WORD_SIZE;

    usize bitmap_bytes = alloc->block_count / CHAR_BIT;

    if (mem_size <= bitmap_bytes)
        return false;

    // Find some space for the bitmap
    void* bitmap_addr = mmap_alloc_inner(mmap, bitmap_bytes, E820_ALLOC, 1, (uptr)-1);
    if (!bitmap_addr)
        return false;

    alloc->bitmap = (bitmap_word*)ID_MAPPED_VADDR(bitmap_addr);

    // Mark the whole bitmap as used
    memset(alloc->bitmap, 0xff, bitmap_bytes);

    for (usize i = 0; i < mmap->count; i++) {
        e820_entry* current = &mmap->entries[i];

        u64 top = current->address + current->size;
        u64 base = current->address;

        if (top > mem_top || base < mem_base)
            continue;

        usize blocks = current->size / block_size;
        usize start_block = _to_block(alloc, (void*)current->address);

        if (current->type == E820_AVAILABLE)
            bitmap_clear_region(alloc->bitmap, start_block, blocks);
        else
            bitmap_set_region(alloc->bitmap, start_block, blocks);
    }

    return true;
}


static usize _first_fit(bitmap_alloc* alloc, usize blocks) {
    usize region_bottom = 0, region_size = 0;

    for (usize word = 0; word < alloc->word_count; word++) {
        if (alloc->bitmap[word] == ~(bitmap_word)0) {
            region_size = 0;
            region_bottom = (word + 1) * BITMAP_WORD_SIZE;
            continue;
        }

        for (usize bit = 0; bit < BITMAP_WORD_SIZE; bit++) {
            if (alloc->bitmap[word] & (1 << bit)) {
                region_size = 0;
                region_bottom = word * BITMAP_WORD_SIZE + bit + 1;
                continue;
            }

            region_size++;

            if (region_size >= blocks)
                return region_bottom;
        }
    }

    return ALLOC_OUT_OF_BLOCKS;
}

void* bitmap_alloc_blocks(bitmap_alloc* alloc, usize blocks) {
    if (blocks == 0)
        return NULL;

    // TODO: implement more allocation strategies
    usize first_block = _first_fit(alloc, blocks);

    if (first_block == ALLOC_OUT_OF_BLOCKS)
        return NULL;

    bitmap_set_region(alloc->bitmap, first_block, blocks);

    return _to_ptr(alloc, first_block);
}

void bitmap_alloc_free(bitmap_alloc* alloc, void* ptr, usize size) {
    if (!ptr || !size)
        return;

    usize first_block = _to_block(alloc, ptr);
    usize count = DIV_ROUND_UP(size, alloc->block_size);

    bitmap_clear_region(alloc->bitmap, first_block, count);
}
