#pragma once

#include <stdbool.h>
#include <sys/types.h>

typedef struct vfs_node vfs_node_t;

bool procfs_init(void);
void procfs_register_pid(pid_t pid);
void procfs_unregister_pid(pid_t pid);
bool procfs_stat_owner(vfs_node_t *node, uid_t *uid_out, gid_t *gid_out);
