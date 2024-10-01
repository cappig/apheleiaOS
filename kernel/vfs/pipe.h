#pragma once

#include <base/types.h>

#include "fs.h"


vfs_node* pipe_create(char* name, usize size);
void pipe_destroy(vfs_node* pipe);
