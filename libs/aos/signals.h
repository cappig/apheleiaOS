#pragma once

#define SIG_ERR 0
#define SIG_DFL 1
#define SIG_IGN 2

#define EXIT_SIGNAL_BASE 128

typedef void (*sighandler_t)(int signum);
typedef sighandler_t sighandler_fn;

enum signal_nums {
    SIGCHLD = 1,

    SIGUSR1 = 2,
    SIGUSR2 = 3,

    SIGINT = 4,
    SIGHUP = 5,

    SIGFPE = 6,
    SIGILL = 7,
    SIGSYS = 8,
    SIGSEGV = 9,
    SIGTERM = 10,
    SIGABRT = 11,

    SIGKILL = 12,

    SIGNAL_COUNT
};
