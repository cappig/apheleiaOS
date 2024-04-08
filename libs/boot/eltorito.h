#pragma once

#include <base/types.h>

#define ELTORITO_TABLE_OFFSET 8

typedef struct {
    u32 pvd_lba;
    u32 boot_lba;
    u32 boot_len;
    u32 checksum;
    u8 _reserved[40];
} eltorito_boot_table;
