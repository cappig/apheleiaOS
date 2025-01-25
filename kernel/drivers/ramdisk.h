#pragma once

#include <base/types.h>

#include "sys/disk.h"

typedef struct {
    bool write;
    void* addr;
    usize size;
} ramdisk_private;

// Provide a vfs interface for reading and writing to a piece of contiguous ram

disk_dev* ramdisk_init(char* name, void* addr, usize size, bool write);
