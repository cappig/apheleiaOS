#pragma once

#include <sys/types.h>

#define PROC_NAME_MAX 32

typedef enum {
    PROC_STATE_READY,
    PROC_STATE_RUNNING,
    PROC_STATE_SLEEPING,
    PROC_STATE_STOPPED,
    PROC_STATE_ZOMBIE,
} proc_state_t;

typedef struct proc_info {
    pid_t pid;
    pid_t ppid;
    pid_t pgid;
    pid_t sid;
    uid_t uid;
    gid_t gid;
    proc_state_t state;
    int tty_index;
    char name[PROC_NAME_MAX];
} proc_info_t;

#ifndef _KERNEL
ssize_t getprocs(proc_info_t* out, size_t capacity);
#endif
