#pragma once

#include <base/types.h>

#include "fs.h"


typedef struct {
    u8* buffer;
    usize size;
    usize current_index;
} vfs_pipe;


vfs_node* pipe_create(char* name, usize size);
void pipe_destroy(vfs_node* pipe);
