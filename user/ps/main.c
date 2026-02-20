#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/proc.h>
#include <unistd.h>

#define PS_COL_PID   5
#define PS_COL_TTY   5
#define PS_COL_STAT  4
#define PS_COL_TIME  5

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
    DIR *proc_dir = opendir("/proc");
    if (!proc_dir) {
        write_cstr("ps: failed to open /proc\n");
        return 1;
    }

    char line[192];
    snprintf(
        line,
        sizeof(line),
        "%-*s  %-*s  %-*s  %-*s  %s\n",
        PS_COL_PID,
        "PID",
        PS_COL_TTY,
        "TTY",
        PS_COL_STAT,
        "STAT",
        PS_COL_TIME,
        "TIME",
        "COMMAND"
    );
    write_cstr(line);

    struct dirent *ent = NULL;
    while ((ent = readdir(proc_dir)) != NULL) {
        if (!is_pid_name(ent->d_name)) {
            continue;
        }

        char stat_path[80];
        snprintf(stat_path, sizeof(stat_path), "/proc/%s/stat", ent->d_name);

        proc_stat_t info = {0};
        if (proc_stat_read_path(stat_path, &info) < 0) {
            continue;
        }

        const char *cmd = info.name[0] ? info.name : "thread";

        char tty_buf[8];
        const char *tty = tty_name(&info, tty_buf, sizeof(tty_buf));
        char time_buf[16];
        format_cpu_time_ms(info.cpu_time_ms, time_buf, sizeof(time_buf));

        snprintf(
            line,
            sizeof(line),
            "%-*lld  %-*.*s  %-*.*s  %-*.*s  %s\n",
            PS_COL_PID,
            (long long)info.pid,
            PS_COL_TTY,
            PS_COL_TTY,
            tty,
            PS_COL_STAT,
            PS_COL_STAT,
            state_name(info.state),
            PS_COL_TIME,
            PS_COL_TIME,
            time_buf,
            cmd
        );

        write_cstr(line);
    }

    closedir(proc_dir);

    return 0;
}
