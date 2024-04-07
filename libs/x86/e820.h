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
