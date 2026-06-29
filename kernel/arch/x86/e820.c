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

static bool _mergeable_type(u32 type) {
    return type < E820_ALLOC;
}

void mmap_remove_entry(e820_map_t *map, size_t index) {
    map->count--;

    for (size_t i = index; i < map->count; i++) {
        map->entries[i] = map->entries[i + 1];
    }

    map->entries[map->count] = (e820_entry_t){ 0 };
}

bool mmap_add_entry(e820_map_t *map, u64 address, u64 size, u32 type) {
    if (!map || !size || map->count >= E820_MAX) {
        return false;
    }

    map->entries[map->count] = (e820_entry_t){ address, size, type, 0 };
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

void clean_mmap(e820_map_t *map) {
    if (map->count > E820_MAX) {
        map->count = E820_MAX;
    }

    e820_entry_t *entries = (e820_entry_t *)&map->entries;

    bool changed = true;
    while (changed) {
        changed = false;

        for (size_t i = 0; i < map->count;) {
            if (!entries[i].size) {
                mmap_remove_entry(map, i);
                changed = true;
                continue;
            }

            i++;
        }

        qsort(entries, map->count, sizeof(e820_entry_t), _comp_mmap);

        for (size_t i = 0; i + 1 < map->count; i++) {
            e820_entry_t *left = &entries[i];
            e820_entry_t *right = &entries[i + 1];

            u64 left_top = _region_top(left->address, left->size);
            u64 right_top = _region_top(right->address, right->size);

            if (left_top < right->address) {
                continue;
            }

            bool touching = left_top == right->address;
            if (touching && left->type != right->type) {
                continue;
            }

            if (left->type == right->type && _mergeable_type(left->type)) {
                u64 merged_top = max(left_top, right_top);
                left->size = merged_top - left->address;
                mmap_remove_entry(map, i + 1);
                changed = true;
                break;
            }

            if (touching) {
                continue;
            }

            if (left->type > right->type) {
                if (right_top <= left_top) {
                    mmap_remove_entry(map, i + 1);
                } else {
                    right->address = left_top;
                    right->size = right_top - left_top;
                }

                changed = true;
                break;
            }

            u64 left_base = left->address;
            u64 right_base = right->address;

            if (right_base > left_base) {
                left->size = right_base - left_base;

                if (right_top < left_top) {
                    (void)mmap_add_entry(map, right_top, left_top - right_top, left->type);
                }
            } else if (right_top < left_top) {
                left->address = right_top;
                left->size = left_top - right_top;
            } else {
                mmap_remove_entry(map, i);
            }

            changed = true;
            break;
        }
    }
}

void *mmap_alloc_inner(e820_map_t *mmap, size_t bytes, u32 type, u32 alignment, u64 top) {
    if (!bytes) {
        return NULL;
    }

    e820_entry_t *entries = (e820_entry_t *)&mmap->entries;

    // an alignment of 0 means 'do not align'
    if (!alignment) {
        alignment = 1;
    }

    if (!top) {
        top = (u64)-1;
    }

    // protected mode can't handle addresses larger than 4 Gib
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

        // only map conventional memory if asked explicitly to do so
        if (top > 0xfffff && entry_top <= 0xfffff) {
            continue;
        }

        u64 base = ALIGN_DOWN(entry_top - bytes, alignment);

        if (base < entries[i].address) {
            continue;
        }

        u64 old_addr = entries[i].address;
        u64 old_size = entries[i].size;
        u32 old_type = entries[i].type;
        u32 old_acpi = entries[i].acpi;
        size_t old_count = mmap->count;
        u64 old_top = _region_top(old_addr, old_size);
        u64 alloc_top = base + bytes;

        entries[i] = (e820_entry_t){ 0 };

        if (base > old_addr) {
            entries[i] = (e820_entry_t){ old_addr, base - old_addr, old_type, old_acpi };
        } else if (old_top > alloc_top) {
            entries[i] = (e820_entry_t){ alloc_top, old_top - alloc_top, old_type, old_acpi };
        }

        if (base > old_addr && old_top > alloc_top) {
            if (!mmap_add_entry(mmap, alloc_top, old_top - alloc_top, old_type)) {
                entries[i] = (e820_entry_t){ old_addr, old_size, old_type, old_acpi };
                mmap->count = old_count;
                return NULL;
            }
        }

        if (!mmap_add_entry(mmap, base, (u64)bytes, type)) {
            entries[i] = (e820_entry_t){ old_addr, old_size, old_type, old_acpi };
            mmap->count = old_count;
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
            current->type = E820_AVAILABLE;
            current->acpi = 0;
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
        return "boot allocation";
    case E820_PAGE_TABLE:
        return "page tables";
    case E820_KERNEL:
        return "kernel data";
    default:
        return "unknown";
    }
}

typedef struct {
    u64 base;
    u64 top;
} e820_span_t;

static bool _clip_to_boot_range(const e820_entry_t *entry, e820_span_t *out) {
    u64 base = entry->address;
    u64 top = _region_top(entry->address, entry->size);

    // we only map the low 4 GiB in the current setup
    if (base >= PROTECTED_MODE_TOP) {
        return false;
    }

    if (top > PROTECTED_MODE_TOP) {
        top = PROTECTED_MODE_TOP;
    }

    if (top <= base) {
        return false;
    }

    *out = (e820_span_t){ .base = base, .top = top };
    return true;
}

static bool _managed_span(e820_map_t *mmap, size_t block_size, e820_span_t *out) {
    u64 base = (u64)-1;
    u64 top = 0;

    for (size_t i = 0; i < mmap->count; i++) {
        e820_entry_t *entry = &mmap->entries[i];

        if (entry->type != E820_AVAILABLE) {
            continue;
        }

        e820_span_t span = { 0 };
        if (!_clip_to_boot_range(entry, &span)) {
            continue;
        }

        if (base > span.base) {
            base = span.base;
        }

        if (top < span.top) {
            top = span.top;
        }
    }

    if (base == (u64)-1 || top <= base) {
        return false;
    }

    if (base < MIB) {
        base = MIB;
    }

    base = ALIGN(base, block_size);
    top = ALIGN_DOWN(top, block_size);
    if (top <= base) {
        return false;
    }

    *out = (e820_span_t){ .base = base, .top = top };
    return true;
}

static void *_reserve_bitmap(e820_map_t *mmap, size_t block_size, size_t bitmap_size) {
    return mmap_alloc_inner(mmap, bitmap_size, E820_ALLOC, (u32)block_size, PROTECTED_MODE_TOP);
}

static void _mark_bitmap_blocks(bitmap_allocator_t *alloc, e820_map_t *mmap, e820_span_t mem, size_t block_size) {
    for (size_t i = 0; i < mmap->count; i++) {
        e820_entry_t *entry = &mmap->entries[i];

        e820_span_t span = { 0 };
        if (!_clip_to_boot_range(entry, &span)) {
            continue;
        }

        if (span.top <= mem.base || span.base >= mem.top) {
            continue;
        }

        if (span.base < mem.base) {
            span.base = mem.base;
        }

        if (span.top > mem.top) {
            span.top = mem.top;
        }

        if (entry->type == E820_AVAILABLE) {
            span.base = ALIGN(span.base, block_size);
            span.top = ALIGN_DOWN(span.top, block_size);
        } else {
            span.base = ALIGN_DOWN(span.base, block_size);
            span.top = ALIGN(span.top, block_size);

            if (span.base < mem.base) {
                span.base = mem.base;
            }

            if (span.top > mem.top) {
                span.top = mem.top;
            }
        }

        if (span.top <= span.base) {
            continue;
        }

        size_t blocks = (size_t)((span.top - span.base) / block_size);
        if (!blocks) {
            continue;
        }

        size_t start_block = bitmap_alloc_to_block(alloc, (void *)(uintptr_t)span.base);

        if (entry->type == E820_AVAILABLE) {
            alloc->free_blocks += blocks;
            alloc->usable_blocks += blocks;
            bitmap_clear_region(alloc->bitmap, start_block, blocks);
        } else {
            bitmap_set_region(alloc->bitmap, start_block, blocks);
        }
    }
}

bool bitmap_alloc_init_mmap(bitmap_allocator_t *alloc, e820_map_t *mmap, size_t block_size) {
    if (mmap->count > E820_MAX) {
        mmap->count = E820_MAX;
    }

    e820_span_t mem = { 0 };
    if (!_managed_span(mmap, block_size, &mem)) {
        return false;
    }

    u64 mem_size = mem.top - mem.base;
    alloc->chunk_start = (void *)(uintptr_t)mem.base;
    alloc->chunk_size = (size_t)mem_size;

    alloc->block_size = block_size;
    alloc->block_count = (size_t)(mem_size / block_size);
    alloc->word_count = DIV_ROUND_UP(alloc->block_count, BITMAP_WORD_SIZE);

    size_t bitmap_bytes = DIV_ROUND_UP(alloc->block_count, CHAR_BIT);
    size_t bitmap_size = ALIGN(bitmap_bytes, block_size);

    if (mem_size <= bitmap_size) {
        return false;
    }

    void *bitmap_addr = _reserve_bitmap(mmap, block_size, bitmap_size);

    if (!bitmap_addr) {
        return false;
    }

    // the allocator tracks physical addresses, but the bitmap itself must be
    // accessed via a valid virtual mapping
#if defined(__x86_64__)
    alloc->bitmap = (bitmap_word_t *)((uintptr_t)bitmap_addr + LINEAR_MAP_OFFSET_64);
#else
    alloc->bitmap = (bitmap_word_t *)(bitmap_addr);
#endif

    // mark the whole bitmap as used
    memset(alloc->bitmap, (unsigned int)-1, bitmap_size);
    alloc->free_blocks = 0;
    alloc->usable_blocks = 0;

    _mark_bitmap_blocks(alloc, mmap, mem, block_size);
    return true;
}
