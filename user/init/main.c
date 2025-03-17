#include <aos/signals.h>
#include <aos/syscalls.h>
#include <stdlib.h>
#include <string.h>


void child(int signum) {
    for (;;) {
        int status = 0;
        pid_t pid = sys_wait(-1, &status, WNOHANG);

        if (pid <= 0)
            break;

        char emsg[] = "child exited with code: ";
        sys_write(STDOUT_FD, emsg, strlen(emsg));

        char ecode[] = "      \n";
        itoa(status, ecode, 10);
        sys_write(STDOUT_FD, ecode, strlen(ecode));
    }
}


// int main(int argc, char* argv[], char* envp[]) {
int main(void) {
    char buf[] = "Hello from userland!\n";
    sys_write(STDOUT_FD, buf, strlen(buf));

    sys_signal(SIGCHLD, child);

    pid_t pid = sys_fork();

    if (!pid)
        sys_exit(128);

    for (;;) {}

    return 0;
}
