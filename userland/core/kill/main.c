#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static bool parse_number(const char *text, long min, long max, long *out) {
    if (!text || !*text || !out) {
        return false;
    }

    errno = 0;
    char *end = NULL;
    long value = strtol(text, &end, 10);

    if (errno == ERANGE || end == text || *end || value < min || value > max) {
        return false;
    }

    *out = value;
    return true;
}

static int parse_signal(const char *arg) {
    if (!arg || !*arg) {
        return -1;
    }

    if (arg[0] == '-') {
        arg++;
        if (!*arg) {
            return -1;
        }
    }

    long sig = 0;
    if (parse_number(arg, 0, NSIG - 1, &sig)) {
        return (int)sig;
    }

    return -1;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        static const char usage[] = "usage: kill [-s SIG] [--] PID...\n";
        write(STDERR_FILENO, usage, sizeof(usage) - 1);
        return 1;
    }

    int sig = SIGTERM;
    int argi = 1;

    if (!strcmp(argv[argi], "-s") && argi + 1 < argc) {
        sig = parse_signal(argv[argi + 1]);
        argi += 2;
    } else if (argv[argi][0] == '-') {
        sig = parse_signal(argv[argi]);
        argi += 1;
    }

    if (sig < 0 || sig >= NSIG) {
        static const char error[] = "kill: invalid signal\n";
        write(STDERR_FILENO, error, sizeof(error) - 1);
        return 1;
    }

    if (argi < argc && !strcmp(argv[argi], "--")) {
        argi++;
    }

    if (argi >= argc) {
        static const char error[] = "kill: missing pid\n";
        write(STDERR_FILENO, error, sizeof(error) - 1);
        return 1;
    }

    int status = 0;
    for (; argi < argc; argi++) {
        long value = 0;
        if (!parse_number(argv[argi], INT_MIN + 1L, INT_MAX, &value)) {
            static const char error[] = "kill: invalid pid\n";
            write(STDERR_FILENO, error, sizeof(error) - 1);
            status = 1;
            continue;
        }

        if (kill((pid_t)value, sig) < 0) {
            static const char error[] = "kill: failed\n";
            write(STDERR_FILENO, error, sizeof(error) - 1);
            status = 1;
        }
    }

    return status;
}
