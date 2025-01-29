#pragma once

#include "process.h"


bool exec_elf(process* proc, vfs_node* file, char** argv, char** envp);
