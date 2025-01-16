#include <aos/signals.h>
#include <aos/syscalls.h>
#include <string.h>


void child(int signum) {
    char buf[] = "SIGNAL!\n";
    sys_write(STDOUT_FD, buf, strlen(buf));
}


int main(void) {
    char buf[] = "Hello from userland!\n";
    sys_write(STDOUT_FD, buf, strlen(buf));

    sys_signal(SIGUSR1, child);
    sys_kill(sys_getpid(), SIGUSR1);

    sys_write(STDOUT_FD, buf, strlen(buf));

    for (;;) {}

    return 0;
}
