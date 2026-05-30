#pragma once

#include <stdbool.h>
#include <sys/types.h>

typedef struct vfs_node vfs_node_t;

bool procfs_init(void);
void procfs_register_pid(pid_t pid);
void procfs_unregister_pid(pid_t pid);
void procfs_sweep_dead(void);
bool procfs_stat_owner(vfs_node_t *node, uid_t *uid_out, gid_t *gid_out);
bool procfs_lock_path(const char *path, unsigned long *flags_out);
bool procfs_lock_dir(vfs_node_t *node, unsigned long *flags_out);
void procfs_unlock_dir(bool locked, unsigned long flags);
