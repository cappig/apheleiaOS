#pragma once

#include <base/attributes.h>
#include <base/types.h>

#define E820_MAX 128

typedef enum : u32 {
    E820_AVAILABLE = 1,
    E820_RESERVED = 2,
    E820_ACPI = 3,
    E820_NVS = 4,
    E820_CORRUPTED = 5,

    // Custom types used internaly here by the kernel
    E820_ALLOC = 100, // Temporary allocation
    E820_PAGE_TABLE = 101, // Temporary page tables set up by the bootloader
    E820_KERNEL = 102, // Kernel ELF segments and kernel page tables
} e820_type_t;

typedef struct PACKED {
    u64 address;
    u64 size;
    u32 type;
    u32 acpi;
} e820_entry_t;

typedef struct PACKED {
    u64 count;
    e820_entry_t entries[E820_MAX];
} e820_map_t;


void mmap_remove_entry(e820_map_t* map, size_t index);
void mmap_add_entry(e820_map_t* map, u64 address, u64 size, u32 type);

void clean_mmap(e820_map_t* map);

void* mmap_alloc_inner(e820_map_t* mmap, size_t bytes, u32 type, u32 alignment, u64 top);
bool mmap_free_inner(e820_map_t* map, void* ptr);

char* mem_map_type_string(e820_type_t type);
void dump_map(e820_map_t* map);
