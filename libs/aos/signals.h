#pragma once

#define SIG_ERR ((sighandler_fn)(-1))
#define SIG_DFL ((sighandler_fn)0)
#define SIG_IGN ((sighandler_fn)1)

#define EXIT_SIGNAL_BASE 127

typedef void (*sighandler_t)(int signum);
typedef sighandler_t sighandler_fn;

enum signal_nums {
    SIGNAL_NONE = 0,

    SIGCHLD = 1,

    SIGUSR1 = 2,
    SIGUSR2 = 3,

    SIGQUIT = 4,
    SIGINT = 5,
    SIGHUP = 6,
    SIGTSTP = 8,

    SIGTRAP = 7,

    SIGTERM = 9,
    SIGABRT = 10,

    SIGCONT = 11,

    SIGSYS = 12,
    SIGFPE = 13,
    SIGILL = 14,
    SIGBUS = 15,
    SIGSEGV = 16,

    SIGSTOP = 17,

    SIGKILL = 18,

    SIGNAL_COUNT // SIGNAL_COUNT-1 should be used since we index signals from 1
};
