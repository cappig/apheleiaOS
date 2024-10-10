#pragma once

#include <base/addr.h>
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
} disk_parameters;

typedef struct PACKED {
    u8 size;
    u8 _unused0;
    u16 sectors;
    u32_addr destination;
    u64 lba;
} disk_address_packet;

typedef struct {
    void* addr;
    usize size;
} file_handle;


isize read_disk(void* dest, usize offset, usize bytes);

void init_disk(u16 disk_code);

void open_root_file(file_handle* file, const char* name);
void close_root_file(file_handle* file);
