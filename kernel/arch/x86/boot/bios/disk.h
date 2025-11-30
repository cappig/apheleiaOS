#pragma once

#include <base/attributes.h>
#include <base/types.h>

typedef struct PACKED {
    u16 size;
    u16 flags;
    u32 cylinders;
    u32 heads;
    u32 sectors_per_track;
    u64 sectors_count;
    u16 bytes_per_sector;
    u32 edd; // optional
} disk_params_t;

typedef struct PACKED {
    u8 size;
    u8 _unused0;
    u16 sectors;
    u32 destination;
    u64 lba;
} dap_t;


int read_disk(void* dest, size_t offset, size_t bytes);

void disk_init(u16 disk);

void* read_rootfs(const char* path);
