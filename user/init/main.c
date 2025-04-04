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

    /* sys_signal(SIGCHLD, child); */
    /**/
    /* pid_t pid = sys_fork(); */
    /**/
    /* if (!pid) */
    /*     sys_exit(128); */

    u8* ret = sys_mmap(0, 0x1000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);

    char ecode[] = "                 \n";
    itoa((u64)ret, ecode, 16);
    sys_write(STDOUT_FD, ecode, strlen(ecode));

    for (;;) {} // init should never ever exit

    return 0;
}
