#pragma once

#include <signal.h>
#include <sys/types.h>

sighandler_t signal(int signum, sighandler_t handler);
int kill(pid_t pid, int signum);
int raise(int signum);
