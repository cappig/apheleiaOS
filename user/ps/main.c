#include <stdio.h>
#include <string.h>
#include <sys/proc.h>
#include <unistd.h>

static void write_cstr(const char* text) {
    if (!text)
        return;

    write(STDOUT_FILENO, text, strlen(text));
}

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

static const char* tty_name(const proc_info_t* info, char* buf, size_t buf_len) {
    if (!info)
        return "??";

    if (info->tty_index < 0)
        return "??";

    if (!info->tty_index)
        return "console";

    snprintf(buf, buf_len, "tty%d", info->tty_index - 1);
    return buf;
}

int main(void) {
    proc_info_t procs[128];
    ssize_t count = getprocs(procs, sizeof(procs) / sizeof(procs[0]));

    if (count < 0) {
        write_cstr("ps: getprocs failed\n");
        return 1;
    }

    write_cstr("PID   TTY     STAT TIME COMMAND\n");

    for (ssize_t i = 0; i < count; i++) {
        proc_info_t* info = &procs[i];

        const char* cmd = info->name[0] ? info->name : "thread";

        char tty_buf[8];
        const char* tty = tty_name(info, tty_buf, sizeof(tty_buf));

        char line[160];
        snprintf(
            line,
            sizeof(line),
            "%-5lld %-7s %-4s %-4s %s\n",
            (long long)info->pid,
            tty,
            state_name(info->state),
            "0:00",
            cmd
        );

        write_cstr(line);
    }

    return 0;
}
