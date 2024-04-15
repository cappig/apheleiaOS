#pragma once

#include <base/attributes.h>
#include <base/types.h>

#define KERNEL_HEAP_BLOCK 8

#define KERNEL_HEAP_MAGIC 0xA110ca7e

typedef struct PACKED {
    u32 magic; // 4 bytes = 8 hex digits
    u32 size; // in blocks
} kheap_header;


void heap_init(void);

void* kmalloc(usize size);
void* kcalloc(usize size);
void kfree(void* ptr);

void galloc_init(void);
