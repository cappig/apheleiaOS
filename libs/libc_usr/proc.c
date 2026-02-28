#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/proc.h>
#include <unistd.h>

#define PROC_STAT_TEXT_MAX 512

static bool _parse_i64(const char *text, long long *out) {
    if (!text || !out || !text[0]) {
        return false;
    }

    char *end = NULL;
    long long value = strtoll(text, &end, 10);

    if (end == text || *end != '\0') {
        return false;
    }

    *out = value;
    return true;
}

static bool _parse_u64(const char *text, unsigned long long *out) {
    if (!text || !out || !text[0]) {
        return false;
    }

    char *end = NULL;
    long long value = strtoll(text, &end, 10);

    if (end == text || *end != '\0' || value < 0) {
        return false;
    }

    *out = (unsigned long long)value;
    return true;
}

int proc_stat_parse(const char *text, proc_stat_t *out) {
    if (!text || !out) {
        errno = EINVAL;
        return -1;
    }

    memset(out, 0, sizeof(*out));
    out->state = PROC_STATE_UNKNOWN;
    out->tty_index = PROC_TTY_NONE;

    bool have_pid = false;
    const char *line = text;

    while (*line) {
        const char *end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line) : strlen(line);

        if (len) {
            const char *sep = memchr(line, '=', len);
            if (sep) {
                size_t key_len = (size_t)(sep - line);
                size_t value_len = len - key_len - 1;

                char key[24];
                char value[PROC_STAT_TEXT_MAX];

                if (key_len >= sizeof(key)) {
                    key_len = sizeof(key) - 1;
                }

                if (value_len >= sizeof(value)) {
                    value_len = sizeof(value) - 1;
                }

                memcpy(key, line, key_len);
                key[key_len] = '\0';

                memcpy(value, sep + 1, value_len);
                value[value_len] = '\0';

                if (!strcmp(key, "pid")) {
                    long long parsed = 0;
                    if (_parse_i64(value, &parsed)) {
                        out->pid = (pid_t)parsed;
                        have_pid = true;
                    }
                } else if (!strcmp(key, "ppid")) {
                    long long parsed = 0;
                    if (_parse_i64(value, &parsed)) {
                        out->ppid = (pid_t)parsed;
                    }
                } else if (!strcmp(key, "pgid")) {
                    long long parsed = 0;
                    if (_parse_i64(value, &parsed)) {
                        out->pgid = (pid_t)parsed;
                    }
                } else if (!strcmp(key, "sid")) {
                    long long parsed = 0;
                    if (_parse_i64(value, &parsed)) {
                        out->sid = (pid_t)parsed;
                    }
                } else if (!strcmp(key, "uid")) {
                    long long parsed = 0;
                    if (_parse_i64(value, &parsed)) {
                        out->uid = (uid_t)parsed;
                    }
                } else if (!strcmp(key, "gid")) {
                    long long parsed = 0;
                    if (_parse_i64(value, &parsed)) {
                        out->gid = (gid_t)parsed;
                    }
                } else if (!strcmp(key, "signal_pending")) {
                    unsigned long long parsed = 0;
                    if (_parse_u64(value, &parsed)) {
                        out->signal_pending = (uint32_t)parsed;
                    }
                } else if (!strcmp(key, "state")) {
                    out->state = value[0] ? value[0] : PROC_STATE_UNKNOWN;
                } else if (!strcmp(key, "tty_index")) {
                    long long parsed = 0;
                    if (_parse_i64(value, &parsed)) {
                        out->tty_index = (int)parsed;
                    }
                } else if (!strcmp(key, "cpu_time_ms")) {
                    unsigned long long parsed = 0;
                    if (_parse_u64(value, &parsed)) {
                        out->cpu_time_ms = (uint64_t)parsed;
                    }
                } else if (!strcmp(key, "name")) {
                    strncpy(out->name, value, sizeof(out->name) - 1);
                }
            }
        }

        if (!end) {
            break;
        }

        line = end + 1;
    }

    if (!have_pid) {
        errno = EINVAL;
        return -1;
    }

    return 0;
}

ssize_t proc_stat_read_path(const char *path, proc_stat_t *out) {
    if (!path || !out) {
        errno = EINVAL;
        return -1;
    }

    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) {
        return -1;
    }

    char text[PROC_STAT_TEXT_MAX];
    size_t used = 0;

    while (used + 1 < sizeof(text)) {
        ssize_t rd = read(fd, text + used, sizeof(text) - 1 - used);
        if (rd < 0) {
            int saved = errno;
            close(fd);
            errno = saved;
            return -1;
        }

        if (!rd) {
            break;
        }

        used += (size_t)rd;
    }

    close(fd);
    text[used] = '\0';

    if (proc_stat_parse(text, out) < 0) {
        return -1;
    }

    return (ssize_t)used;
}

ssize_t proc_stat_read(pid_t pid, proc_stat_t *out) {
    if (pid <= 0 || !out) {
        errno = EINVAL;
        return -1;
    }

    char path[40];
    snprintf(path, sizeof(path), "/proc/%lld/stat", (long long)pid);
    return proc_stat_read_path(path, out);
}
