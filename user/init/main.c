#include <aos/signals.h>
#include <aos/syscalls.h>
#include <stdlib.h>
#include <string.h>


static void advance_cursor() {
    static int pos = 0;

    char cursor[4] = {'/', '-', '\\', '|'};

    char buf[] = "X\b";
    buf[0] = cursor[pos];

    sys_write(STDOUT_FD, buf, strlen(buf));

    pos = (pos + 1) % 4;
}


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

    char buf2[] = "Like a record, baby, right round, round, round . . . ";
    sys_write(STDOUT_FD, buf2, strlen(buf2));

    pid_t pid = sys_fork();

    if (!pid)
        sys_exit(128);

    for (;;) {
        advance_cursor();
        sys_sleep(100);
    }

    for (;;) {}

    return 0;
}
