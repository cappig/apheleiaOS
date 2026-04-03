#pragma once

#include <base/types.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    u64 vaddr;
    u64 paddr;
    u64 offset;
    u64 file_size;
    u64 mem_size;
    u64 align;
    u32 flags;
} elf_segment_t;

typedef struct {
    u64 entry;
    bool is_64;
} elf_info_t;

typedef bool (*elf_segment_cb)(const elf_segment_t *seg, void *ctx);

bool elf_foreach_segment(
    const void *elf,
    size_t size,
    elf_segment_cb cb,
    void *ctx,
    elf_info_t *out_info
);
