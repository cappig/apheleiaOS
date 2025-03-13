#pragma once

#include "process.h"

bool exec_elf(sched_thread* thread, vfs_node* file, char** argv, char** envp);
