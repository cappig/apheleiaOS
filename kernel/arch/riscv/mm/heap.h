#pragma once

#include <base/attributes.h>
#include <base/types.h>

#define KERNEL_HEAP_BLOCK_SIZE 8
#define KERNEL_HEAP_PAGES      512

#define KERNEL_HEAP_MAGIC 0xA110ca7e

typedef struct PACKED {
    u32 magic;
    u32 size;
} kheap_header_t;

void heap_init(void);
void arch_init_alloc(void);
