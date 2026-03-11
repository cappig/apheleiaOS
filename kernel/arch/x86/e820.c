#include "e820.h"

#include <base/macros.h>
#include <base/types.h>
#include <base/units.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "alloc/bitmap.h"
#include "x86/boot.h"

static inline u64 _region_top(u64 base, u64 size) {
    if (size > UINT64_MAX - base) {
        return UINT64_MAX;
    }

    return base + size;
}


void mmap_remove_entry(e820_map_t *map, size_t index) {
    map->count--;

    for (size_t i = index; i < map->count; i++) {
        map->entries[i] = map->entries[i + 1];
    }

    map->entries[map->count] = (e820_entry_t){0};
}

bool mmap_add_entry(e820_map_t *map, u64 address, u64 size, u32 type) {
    if (!map || !size || map->count >= E820_MAX) {
        return false;
    }

    map->entries[map->count] = (e820_entry_t){address, size, type, 0};
    map->count++;

    return true;
}

static int _comp_mmap(const void *a, const void *b) {
    e820_entry_t *a_elem = (e820_entry_t *)a;
    e820_entry_t *b_elem = (e820_entry_t *)b;

    if (a_elem->address < b_elem->address) {
        return -1;
    }

    if (a_elem->address > b_elem->address) {
        return 1;
    }

    return 0;
}

// Should be called every time the map is altered
void clean_mmap(e820_map_t *map) {
    if (map->count > E820_MAX) {
        map->count = E820_MAX;
    }

    e820_entry_t *entries = (e820_entry_t *)&map->entries;

    qsort(entries, map->count, sizeof(e820_entry_t), _comp_mmap);

    for (size_t i = 0; i < map->count; i++) {
        if (!entries[i].size) {
            mmap_remove_entry(map, i--);
        }

        if (i + 1 >= map->count) {
            continue;
        }

        u64 top = _region_top(entries[i].address, entries[i].size);

        // Not touching or overlapping, skip
        if (top < entries[i + 1].address) {
            continue;
        }

        if (entries[i].type == entries[i + 1].type) {
            // Don't merge allocations so that we can free individual blocks
            if (entries[i].type == E820_ALLOC) {
                continue;
            }

            entries[i + 1].address = entries[i].address;
            entries[i].size = 0;
            mmap_remove_entry(map, i--);
        } else {
            u64 overlap = top - entries[i + 1].address;

            if (!overlap) {
                continue;
            }

            if (entries[i].type > entries[i + 1].type) {
                entries[i + 1].address += overlap;
                entries[i + 1].size -= overlap;
            } else {
                entries[i].size -= overlap;
            }
        }
    }
}

void *mmap_alloc_inner(
    e820_map_t *mmap,
    size_t bytes,
    u32 type,
    u32 alignment,
    u64 top
) {
    if (!bytes) {
        return NULL;
    }

    e820_entry_t *entries = (e820_entry_t *)&mmap->entries;

    // An alignment of 0 means 'do not align'
    if (!alignment) {
        alignment = 1;
    }

    if (!top) {
        top = (u64)-1;
    }

    // Protected mode can't handle addresses larger than 4 Gib
#if defined(__i386__)
    top = min(top, 0x100000000UL);
#endif

    for (size_t i = 0; i < mmap->count; i++) {
        if (entries[i].type != E820_AVAILABLE) {
            continue;
        }

        if (entries[i].size < bytes) {
            continue;
        }

        if (entries[i].address >= top) {
            continue;
        }

        u64 entry_top = _region_top(entries[i].address, entries[i].size);

        if (entry_top > top) {
            entry_top = top;
        }

        // Only map conventional memory if asked explicitly to do so
        if (top > 0xfffff && entry_top <= 0xfffff) {
            continue;
        }

        u64 base = ALIGN_DOWN(entry_top - bytes, alignment);

        if (base < entries[i].address) {
            continue;
        }

        // Shrink current entry
        u64 old_addr = entries[i].address;
        u64 old_size = entries[i].size;
        u64 size = entry_top - base;
        entries[i].address += size;
        entries[i].size -= size;

        // Create new entry with memory taken from the current one
        if (!mmap_add_entry(mmap, base, (u64)bytes, type)) {
            entries[i].address = old_addr;
            entries[i].size = old_size;
            return NULL;
        }

        clean_mmap(mmap);

        return (void *)(uintptr_t)base;
    }

    return NULL;
}

bool mmap_free_inner(e820_map_t *mmap, void *ptr) {
    for (size_t i = 0; i < mmap->count; i++) {
        e820_entry_t *current = &mmap->entries[i];

        if (current->address == (u64)(uintptr_t)ptr) {
            mmap_remove_entry(mmap, i);
            clean_mmap(mmap);

            return 0;
        }
    }

    return 1;
}

char *mem_map_type_string(e820_type_t type) {
    switch (type) {
    case E820_AVAILABLE:
        return "available";
    case E820_RESERVED:
        return "reserved";
    case E820_ACPI:
        return "ACPI reclaimable";
    case E820_NVS:
        return "ACPI NVS";
    case E820_CORRUPTED:
        return "BAD RAM!";
    case E820_ALLOC:
        return "temporary allocation";
    case E820_PAGE_TABLE:
        return "page tables";
    case E820_KERNEL:
        return "kernel data";
    default:
        return "unknown";
    }
}


bool bitmap_alloc_init_mmap(
    bitmap_allocator_t *alloc,
    e820_map_t *mmap,
    size_t block_size
) {
    if (mmap->count > E820_MAX) {
        mmap->count = E820_MAX;
    }

    u64 mem_base = (u64)-1;
    u64 mem_top = 0;
    u64 max_addr = PROTECTED_MODE_TOP;

    for (size_t i = 0; i < mmap->count; i++) {
        e820_entry_t *current = &mmap->entries[i];

        if (current->type != E820_AVAILABLE) {
            continue;
        }

        u64 top = _region_top(current->address, current->size);
        u64 base = current->address;

        // We only map the low 4 GiB in the current setup
        if (base >= max_addr) {
            continue;
        }

        if (top > max_addr) {
            top = max_addr;
        }

        if (mem_base > base) {
            mem_base = base;
        }

        if (mem_top < top) {
            mem_top = top;
        }
    }

    if (mem_base == (u64)-1 || mem_top <= mem_base) {
        return false;
    }

    if (mem_base < MIB) {
        mem_base = MIB;
    }

    if (mem_top <= mem_base) {
        return false;
    }

    mem_base = ALIGN(mem_base, block_size);
    u64 mem_size = mem_top - mem_base;

    alloc->chuck_start = (void *)(uintptr_t)mem_base;
    alloc->chunk_size = (size_t)mem_size;

    alloc->block_size = block_size;
    alloc->block_count = (size_t)(mem_size / block_size);
    alloc->word_count = DIV_ROUND_UP(alloc->block_count, BITMAP_WORD_SIZE);

    size_t bitmap_bytes = DIV_ROUND_UP(alloc->block_count, CHAR_BIT);
    size_t bitmap_size = ALIGN(bitmap_bytes, block_size);

    if (mem_size <= bitmap_size) {
        return false;
    }

    // Find some space for the bitmap (low to avoid clobbering high allocations)
    void *bitmap_addr = NULL;

    for (size_t i = 0; i < mmap->count; i++) {
        e820_entry_t *current = &mmap->entries[i];

        if (current->type != E820_AVAILABLE) {
            continue;
        }

        u64 base = current->address;
        u64 top = _region_top(current->address, current->size);

        if (base < MIB) {
            base = MIB;
        }

        if (base >= max_addr) {
            continue;
        }

        if (top > max_addr) {
            top = max_addr;
        }

        u64 aligned = ALIGN(base, block_size);

        if (aligned + bitmap_size > top) {
            continue;
        }

        bitmap_addr = (void *)(uintptr_t)aligned;

        u64 used_end = aligned + bitmap_size;
        current->address = used_end;
        current->size = top - used_end;

        (void)mmap_add_entry(mmap, aligned, bitmap_size, E820_ALLOC);
        clean_mmap(mmap);
        break;
    }

    if (!bitmap_addr) {
        return false;
    }

    // The allocator tracks physical addresses, but the bitmap itself must be
    // accessed via a valid virtual mapping
#if defined(__x86_64__)
    alloc->bitmap =
        (bitmap_word_t *)((uintptr_t)bitmap_addr + LINEAR_MAP_OFFSET_64);
#else
    alloc->bitmap = (bitmap_word_t *)(bitmap_addr);
#endif

    // Mark the whole bitmap as used
    memset(alloc->bitmap, (unsigned int)-1, bitmap_size);
    alloc->free_blocks = 0;
    alloc->usable_blocks = 0;

    for (size_t i = 0; i < mmap->count; i++) {
        e820_entry_t *current = &mmap->entries[i];

        u64 top = _region_top(current->address, current->size);
        u64 base = current->address;

        if (base >= max_addr) {
            continue;
        }

        if (top > max_addr) {
            top = max_addr;
        }

        if (top <= mem_base || base >= mem_top) {
            continue;
        }

        if (base < mem_base) {
            base = mem_base;
        }

        if (top > mem_top) {
            top = mem_top;
        }

        size_t blocks = (size_t)((top - base) / block_size);
        if (!blocks) {
            continue;
        }

        size_t start_block =
            bitmap_alloc_to_block(alloc, (void *)(uintptr_t)base);

        if (current->type == E820_AVAILABLE) {
            alloc->free_blocks += blocks;
            alloc->usable_blocks += blocks;
            bitmap_clear_region(alloc->bitmap, start_block, blocks);
        } else {
            bitmap_set_region(alloc->bitmap, start_block, blocks);
        }
    }

    return true;
}
