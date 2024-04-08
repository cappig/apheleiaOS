#include "e820.h"

#include <base/addr.h>
#include <base/macros.h>
#include <base/types.h>
#include <stddef.h>
#include <stdlib.h>


void mmap_remove_entry(e820_map* map, usize index) {
    map->count--;

    for (usize i = index; i < map->count; i++)
        map->entries[i] = map->entries[i + 1];

    map->entries[map->count] = (e820_entry){0};
}

void mmap_add_entry(e820_map* map, u64 address, u64 size, u32 type) {
    map->entries[map->count] = (e820_entry){address, size, type, 0};

    map->count++;
}

static int _comp_mmap(const void* a, const void* b) {
    e820_entry* a_elem = (e820_entry*)a;
    e820_entry* b_elem = (e820_entry*)b;

    if (a_elem->address < b_elem->address)
        return -1;
    if (a_elem->address > b_elem->address)
        return 1;

    return 0;
}

// Should be called every time the map is altered
void clean_mmap(e820_map* map) {
    e820_entry* entries = (e820_entry*)&map->entries;

    qsort(entries, map->count, sizeof(e820_entry), _comp_mmap);

    for (usize i = 0; i < map->count; i++) {
        if (entries[i].size == 0)
            mmap_remove_entry(map, i--);

        u64 top = entries[i].address + entries[i].size;

        // Not touching or overlaping, skip
        if (top < entries[i + 1].address)
            continue;

        // Neighbouring regions have the same type, merge them
        if (entries[i].type == entries[i + 1].type) {
            // Don't merge bootloader allocations so that we can free individual blocks
            if (entries[i].type == E820_BOOT_ALLOC)
                continue;

            entries[i + 1].address = entries[i].address;

            entries[i].size = 0;
            mmap_remove_entry(map, i--);
        } else {
            u64 overlap = top - entries[i + 1].address;

            // We don't care if they're touching
            if (overlap == 0)
                continue;

            // Prioritize blocks with the largest `type` value
            if (entries[i].type > entries[i + 1].type) {
                entries[i + 1].address += overlap;
                entries[i + 1].size -= overlap;
            } else {
                entries[i].size -= overlap;
            }
        }
    }
}

void* mmap_alloc_inner(e820_map* mmap, usize bytes, u32 type, uptr top) {
    e820_entry* entries = (e820_entry*)&mmap->entries;

    for (isize i = mmap->count - 1; i >= 0; i--) {
        if (entries[i].type != E820_AVAILABLE)
            continue;

        if (entries[i].size < bytes)
            continue;

#if defined(__i386__)
        // Protected mode (bootloader) can't handle addresses larger than 4 Gib
        if (entries[i].address + entries[i].size >= PROTECTED_MODE_TOP)
            continue;
#endif

        if (entries[i].address + entries[i].size >= top)
            continue;

        u64 base = entries[i].address;

        // Shrink current entry
        entries[i].address += bytes;
        entries[i].size -= bytes;

        // Create new entry with memory taken from the current one
        mmap_add_entry(mmap, base, (u64)bytes, type);

        clean_mmap(mmap);

        return (void*)(uptr)base;
    }

    return NULL;
}

bool mmap_free_inner(e820_map* mmap, void* ptr) {
    for (usize i = 0; i < mmap->count; i++) {
        e820_entry* current = &mmap->entries[i];

        if (current->address == (u64)(uptr)ptr) {
            mmap_remove_entry(mmap, i);
            clean_mmap(mmap);

            return 0;
        }
    }

    return 1;
}
