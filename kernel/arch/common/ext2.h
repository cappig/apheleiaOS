#pragma once

#include <base/types.h>
#include <fs/ext2.h>
#include <stddef.h>

typedef bool (*boot_ext2_read_fn_t)(
    void *dest,
    size_t offset,
    size_t bytes,
    void *ctx
);

typedef struct boot_ext2 {
    boot_ext2_read_fn_t read;
    void *ctx;
    size_t size;
    ext2_superblock_t superblock;
} boot_ext2_t;

bool boot_ext2_init(
    boot_ext2_t *fs,
    boot_ext2_read_fn_t read,
    void *ctx,
    size_t size_limit
);

void *boot_ext2_read_file(
    boot_ext2_t *fs,
    const char *path,
    size_t *out_size
);
