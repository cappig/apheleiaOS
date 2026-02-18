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

// Controlling terminal encoding in proc_info::tty_index.
//  -1: no controlling terminal
//   0: console
//   1..N: virtual tty screen index
// <= -2: pseudo terminal, decoded with PROC_TTY_PTS_INDEX().
#define PROC_TTY_NONE             (-1)
#define PROC_TTY_CONSOLE          0
#define PROC_TTY_PTS(index)       (-2 - (index))
#define PROC_TTY_IS_PTS(value)    ((value) <= -2)
#define PROC_TTY_PTS_INDEX(value) (-(value) - 2)

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
ssize_t getprocs(proc_info_t *out, size_t capacity);
#endif
