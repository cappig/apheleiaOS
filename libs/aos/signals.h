#pragma once

#define SIG_ERR ((sighandler_fn)(-1))
#define SIG_DFL ((sighandler_fn)0)
#define SIG_IGN ((sighandler_fn)1)

#define EXIT_SIGNAL_BASE 128

typedef void (*sighandler_t)(int signum);
typedef sighandler_t sighandler_fn;

enum signal_nums {
    SIGNAL_NONE = 0,

    SIGCHLD = 1,

    SIGUSR1 = 2,
    SIGUSR2 = 3,

    SIGINT = 4,
    SIGHUP = 5,
    SIGTRAP = 6,

    SIGTERM = 7,
    SIGABRT = 8,

    SIGSYS = 9,
    SIGFPE = 10,
    SIGILL = 11,
    SIGBUS = 12,
    SIGSEGV = 13,

    SIGKILL = 14,

    SIGNAL_COUNT // SIGNAL_COUNT-1 should be used since we index signals from 1
};
