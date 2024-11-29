#include "e820.h"

#include <base/addr.h>
#include <base/macros.h>
#include <base/types.h>
#include <inttypes.h>
#include <log/log.h>
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
        if (!entries[i].size)
            mmap_remove_entry(map, i--);

        u64 top = entries[i].address + entries[i].size;

        // Not touching or overlapping, skip
        if (top < entries[i + 1].address)
            continue;

        // Neighboring regions have the same type, merge them
        if (entries[i].type == entries[i + 1].type) {
            // Don't merge allocations so that we can free individual blocks
            if (entries[i].type == E820_ALLOC)
                continue;

            entries[i + 1].address = entries[i].address;

            entries[i].size = 0;
            mmap_remove_entry(map, i--);
        } else {
            u64 overlap = top - entries[i + 1].address;

            // We don't care if they're touching
            if (!overlap)
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

void* mmap_alloc_inner(e820_map* mmap, usize bytes, u32 type, u32 alignment, u64 top) {
    if (!bytes)
        return NULL;

    e820_entry* entries = (e820_entry*)&mmap->entries;

    // An alignment of 0 means 'do not align'
    if (!alignment)
        alignment = 1;

    // Protected mode (bootloader) can't handle addresses larger than 4 Gib
#if defined(__i386__)
    top = min(top, PROTECTED_MODE_TOP);
#endif

    for (usize i = 0; i < mmap->count; i++) {
        if (entries[i].type != E820_AVAILABLE)
            continue;

        if (entries[i].size < bytes)
            continue;

        u64 entry_top = entries[i].address + entries[i].size;

        if (entry_top >= top)
            continue;

        // Only map conventional memory if asked explicitly to do so
        // This is a comparatively small slice of memory that is known be unreliable,
        // for instance writing to it caused a triple fault in VmWare
        if (top > 0xfffff && entry_top <= 0xfffff)
            continue;

        u64 base = ALIGN_DOWN(entry_top - bytes, alignment);

        if (base < entries[i].address)
            continue;

        // Shrink current entry
        u64 size = entry_top - base;
        entries[i].address += size;
        entries[i].size -= size;

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

char* mem_map_type_string(e820_type type) {
    switch (type) {
    case E820_AVAILABLE:
        return "available";
    case E820_RESERVED:
        return "reserved";
    case E820_ACPI:
        return "ACPI reclaimable";
    case E820_NVS:
        return "ACPI NVS";
    case E820_BADRAM:
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

void dump_map(e820_map* map) {
    log_debug("Dump of %u entries in the e820 memory map:", map->count);

    for (usize i = 0; i < map->count; i++) {
        log_debug(
            "[ %#08" PRIx64 " - %#08" PRIx64 " ] %s",
            map->entries[i].address,
            map->entries[i].address + map->entries[i].size - 1,
            mem_map_type_string(map->entries[i].type)
        );
    }
}
