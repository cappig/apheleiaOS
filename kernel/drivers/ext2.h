#pragma once

#include <fs/ext2.h>

typedef struct {
    usize block_size;
    usize block_count;

    usize indirect_block_size;

    ext2_inode* root;

    ext2_superblock* superblock;

    usize group_table_count;
    ext2_group_descriptor* group_table;
} ext2_device_private;


bool ext2_init(void);
