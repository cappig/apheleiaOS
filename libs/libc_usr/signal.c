#include <arch/sys.h>
#include <libc_usr/signal.h>
#include <unistd.h>

static void signal_trampoline(void) {
    syscall0(SYS_SIGRETURN);
    for (;;)
        ;
}

sighandler_t signal(int signum, sighandler_t handler) {
    return (sighandler_t)syscall3(
        SYS_SIGNAL, (uintptr_t)signum, (uintptr_t)handler, (uintptr_t)signal_trampoline
    );
}

int kill(pid_t pid, int signum) {
    return (int)syscall2(SYS_KILL, (uintptr_t)pid, (uintptr_t)signum);
}

int raise(int signum) {
    return kill(getpid(), signum);
}
