#include <stdio.h>
#include <string.h>
#include <sys/proc.h>
#include <unistd.h>

static const char* state_name(proc_state_t state) {
    switch (state) {
    case PROC_STATE_READY:
        return "R";
    case PROC_STATE_RUNNING:
        return "R";
    case PROC_STATE_SLEEPING:
        return "S";
    case PROC_STATE_STOPPED:
        return "T";
    case PROC_STATE_ZOMBIE:
        return "Z";
    default:
        return "?";
    }
}

int main(void) {
    proc_info_t procs[128];
    ssize_t count = getprocs(procs, sizeof(procs) / sizeof(procs[0]));
    if (count < 0) {
        const char* msg = "ps: getprocs failed\n";
        write(STDOUT_FILENO, msg, strlen(msg));
        return 1;
    }

    {
        const char* header = "PID   TTY     STAT TIME COMMAND\n";
        write(STDOUT_FILENO, header, strlen(header));
    }
    for (ssize_t i = 0; i < count; i++) {
        proc_info_t* info = &procs[i];
        const char* name = info->name[0] ? info->name : "thread";
        char tty_buf[8];
        const char* tty = "??";
        if (info->tty_index >= 0) {
            if (info->tty_index == 0) {
                tty = "console";
            } else {
                snprintf(tty_buf, sizeof(tty_buf), "tty%d", info->tty_index - 1);
                tty = tty_buf;
            }
        }
        char line[160];
        snprintf(
            line,
            sizeof(line),
            "%-5lld %-7s %-4s %-4s %s\n",
            (long long)info->pid,
            tty,
            state_name(info->state),
            "0:00",
            name
        );
        write(STDOUT_FILENO, line, strlen(line));
    }

    return 0;
}
