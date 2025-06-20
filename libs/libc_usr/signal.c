#include <signal.h>
#include <sys/types.h>
#include <unistd.h>

#include "errno.h"
#include "unistd.h"


sighandler_t signal(int signum, sighandler_t handler) {
    return (sighandler_t)syscall2(SYS_SIGNAL, signum, (u64)handler);
}


int kill(pid_t pid, int signum) {
    return __SYSCALL_ERRNO(syscall2(SYS_KILL, pid, signum));
}

int raise(int sig) {
    return kill(getpid(), sig);
}
