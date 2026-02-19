#pragma once

#include <stdbool.h>
#include <sys/types.h>

bool procfs_init(void);
void procfs_register_pid(pid_t pid);
void procfs_unregister_pid(pid_t pid);
