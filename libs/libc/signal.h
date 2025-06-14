#pragma once

#define SIG_DFL ((sighandler_t)0)
#define SIG_IGN ((sighandler_t)1)
#define SIG_ERR ((sighandler_t)(-1))

typedef void (*sighandler_t)(int);

typedef int sig_atomic_t;

#define SIGCHLD 1
#define SIGUSR1 2
#define SIGUSR2 3
#define SIGQUIT 4
#define SIGINT  5
#define SIGHUP  6
#define SIGTRAP 7
#define SIGTSTP 8
#define SIGTERM 9
#define SIGABRT 10
#define SIGCONT 11
#define SIGSYS  12
#define SIGFPE  13
#define SIGILL  14
#define SIGBUS  15
#define SIGSEGV 16
#define SIGSTOP 17
#define SIGKILL 18

#define NSIG 32


#ifndef _KERNEL
#include <libc_usr/signal.h>
#endif
