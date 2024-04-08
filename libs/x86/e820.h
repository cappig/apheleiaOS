#pragma once

#include <base/attributes.h>
#include <base/types.h>

#define E820_MAX 128

typedef enum {
    E820_AVAILABLE = 1,
    E820_RESERVED = 2,
    E820_MEM_ACPI = 3,
    E820_MEM_NVS = 4,
    E820_MEM_BADRAM = 5,

    // The types bellow are non standard
    E820_BOOT_ALLOC = 0x100, // Used as heap space for the bootloader allocator
    E820_PAGE_TABLE = 0x101, // Temporary page tables set up by the bootloader
    E820_KERNEL = 0x102, // Kernel ELF segments, kernel page tables etc.
} e820_type;

typedef struct PACKED {
    u64 address;
    u64 size;
    u32 type;
    u32 ahci;
} e820_entry;

typedef struct PACKED {
    u16 count;
    e820_entry entries[E820_MAX];
} e820_map;


void mmap_remove_entry(e820_map* map, usize index);
void mmap_add_entry(e820_map* map, u64 address, u64 size, u32 type);

void clean_mmap(e820_map* map);

void* mmap_alloc_inner(e820_map* map, usize bytes, u32 type, uptr top);
bool mmap_free_inner(e820_map* map, void* ptr);
