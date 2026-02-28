#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/proc.h>
#include <unistd.h>

static int usage(int status) {
    const char *text =
        "usage: tty [-s|--silent]\n"
        "  -s, --silent  print nothing, only set exit status\n";
    int fd = (status == 0) ? STDOUT_FILENO : STDERR_FILENO;
    write(fd, text, strlen(text));
    return status;
}

static const char *tty_path(char *buf, size_t buf_len) {
    if (!buf || buf_len == 0) {
        return "/dev/tty";
    }

    proc_stat_t stat = {0};
    if (proc_stat_read_path("/proc/self/stat", &stat) < 0) {
        snprintf(buf, buf_len, "/dev/tty");
        return buf;
    }

    if (PROC_TTY_IS_PTS(stat.tty_index)) {
        snprintf(buf, buf_len, "/dev/pts%d", PROC_TTY_PTS_INDEX(stat.tty_index));
        return buf;
    }

    if (stat.tty_index == PROC_TTY_CONSOLE) {
        snprintf(buf, buf_len, "/dev/console");
        return buf;
    }

    if (stat.tty_index > 0) {
        snprintf(buf, buf_len, "/dev/tty%d", stat.tty_index - 1);
        return buf;
    }

    snprintf(buf, buf_len, "/dev/tty");
    return buf;
}

int main(int argc, char **argv) {
    bool silent = false;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (!arg) {
            continue;
        }

        if (!strcmp(arg, "-s") || !strcmp(arg, "--silent")) {
            silent = true;
            continue;
        }

        if (!strcmp(arg, "-h") || !strcmp(arg, "--help")) {
            return usage(0);
        }

        return usage(1);
    }

    if (!isatty(STDIN_FILENO)) {
        if (!silent) {
            write(STDOUT_FILENO, "not a tty\n", 10);
        }
        return 1;
    }

    if (!silent) {
        char path[64];
        const char *tty = tty_path(path, sizeof(path));
        write(STDOUT_FILENO, tty, strlen(tty));
        write(STDOUT_FILENO, "\n", 1);
    }

    return 0;
}
