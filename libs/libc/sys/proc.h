#pragma once

#ifndef _APHELEIA_SOURCE
#error "<sys/proc.h> is an apheleiaOS extension. Define _APHELEIA_SOURCE."
#endif

#include <stdbool.h>
#include <sys/types.h>

#define PROC_NAME_MAX 32

// Controlling terminal encoding in procfs stat `tty_index`
//  -1: no controlling terminal
//   0: console
//   1..N: virtual tty screen index
// <= -2: pseudo terminal, decoded with PROC_TTY_PTS_INDEX()
#define PROC_TTY_NONE             (-1)
#define PROC_TTY_CONSOLE          0
#define PROC_TTY_PTS(index)       (-2 - (index))
#define PROC_TTY_IS_PTS(value)    ((value) <= -2)
#define PROC_TTY_PTS_INDEX(value) (-(value) - 2)

#ifndef _KERNEL
typedef enum {
    PROC_STATE_RUNNING = 'R',
    PROC_STATE_SLEEPING = 'S',
    PROC_STATE_STOPPED = 'T',
    PROC_STATE_ZOMBIE = 'Z',
    PROC_STATE_UNKNOWN = '?',
} proc_state_t;

typedef struct {
    pid_t pid;
    pid_t ppid;
    pid_t pgid;
    pid_t sid;
    uid_t uid;
    gid_t gid;
    char state;
    int tty_index;
    uint64_t cpu_time_ms;
    char name[PROC_NAME_MAX];
} proc_stat_t;

int proc_stat_parse(const char *text, proc_stat_t *out);
ssize_t proc_stat_read(pid_t pid, proc_stat_t *out);
ssize_t proc_stat_read_path(const char *path, proc_stat_t *out);
#endif
