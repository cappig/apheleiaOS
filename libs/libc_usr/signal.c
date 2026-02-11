#include <arch/sys.h>
#include <errno.h>
#include <libc_usr/signal.h>
#include <unistd.h>

static void signal_trampoline(void) {
    syscall0(SYS_SIGRETURN);
    for (;;)
        ;
}

sighandler_t signal(int signum, sighandler_t handler) {
    long ret = syscall3(
        SYS_SIGNAL, (uintptr_t)signum, (uintptr_t)handler, (uintptr_t)signal_trampoline
    );
    if (ret < 0) {
        errno = (int)-ret;
        return SIG_ERR;
    }
    return (sighandler_t)ret;
}

int kill(pid_t pid, int signum) {
    return (int)__SYSCALL_ERRNO(syscall2(SYS_KILL, (uintptr_t)pid, (uintptr_t)signum));
}

int raise(int signum) {
    return kill(getpid(), signum);
}
