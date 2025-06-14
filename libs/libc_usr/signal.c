#include <signal.h>
#include <unistd.h>


sighandler_t signal(int signum, sighandler_t handler) {
    return (sighandler_t)syscall2(SYS_SIGNAL, signum, (u64)handler);
}
