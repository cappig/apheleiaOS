#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/proc.h>
#include <unistd.h>

static void write_cstr(const char *text) {
    if (!text) {
        return;
    }

    write(STDOUT_FILENO, text, strlen(text));
}

static const char *state_name(char state) {
    switch (state) {
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

static const char *tty_name(const proc_stat_t *info, char *buf, size_t buf_len) {
    if (!info) {
        return "??";
    }

    if (PROC_TTY_IS_PTS(info->tty_index)) {
        snprintf(buf, buf_len, "pts%d", PROC_TTY_PTS_INDEX(info->tty_index));
        return buf;
    }

    if (info->tty_index == PROC_TTY_NONE) {
        return "??";
    }

    if (info->tty_index == PROC_TTY_CONSOLE) {
        return "console";
    }

    snprintf(buf, buf_len, "tty%d", info->tty_index - 1);

    return buf;
}

static bool is_pid_name(const char *name) {
    if (!name || !name[0]) {
        return false;
    }

    for (const char *p = name; *p; p++) {
        if (!isdigit((unsigned char)*p)) {
            return false;
        }
    }

    return true;
}

static void format_cpu_time_ms(uint64_t total_ms, char *buf, size_t buf_len) {
    if (!buf || !buf_len) {
        return;
    }

    uint64_t total_secs = total_ms / 1000ULL;
    uint64_t hours = total_secs / 3600ULL;
    uint64_t minutes = (total_secs / 60ULL) % 60ULL;
    uint64_t seconds = total_secs % 60ULL;

    if (hours) {
        snprintf(
            buf,
            buf_len,
            "%llu:%02llu:%02llu",
            (unsigned long long)hours,
            (unsigned long long)minutes,
            (unsigned long long)seconds
        );
        return;
    }

    snprintf(
        buf,
        buf_len,
        "%llu:%02llu",
        (unsigned long long)(total_secs / 60ULL),
        (unsigned long long)seconds
    );
}

int main(void) {
    int proc_fd = open("/proc", O_RDONLY, 0);
    if (proc_fd < 0) {
        write_cstr("ps: failed to open /proc\n");
        return 1;
    }

    write_cstr("PID   TTY     STAT TIME COMMAND\n");

    dirent_t ent;
    while (getdents(proc_fd, &ent) > 0) {
        if (!is_pid_name(ent.d_name)) {
            continue;
        }

        char stat_path[80];
        snprintf(stat_path, sizeof(stat_path), "/proc/%s/stat", ent.d_name);

        proc_stat_t info = {0};
        if (proc_stat_read_path(stat_path, &info) < 0) {
            continue;
        }

        const char *cmd = info.name[0] ? info.name : "thread";

        char tty_buf[8];
        const char *tty = tty_name(&info, tty_buf, sizeof(tty_buf));
        char time_buf[16];
        format_cpu_time_ms(info.cpu_time_ms, time_buf, sizeof(time_buf));

        char line[160];
        snprintf(
            line,
            sizeof(line),
            "%-5lld %-7s %-4s %-8s %s\n",
            (long long)info.pid,
            tty,
            state_name(info.state),
            time_buf,
            cmd
        );

        write_cstr(line);
    }

    close(proc_fd);

    return 0;
}
