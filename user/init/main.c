#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

void child(int signum) {
    for (;;) {
        int status = 0;
        pid_t pid = waitpid(-1, &status, WNOHANG);

        if (pid <= 0)
            break;

        printf("child %zd exited with code: %d\n", pid, status);
    }
}


// int main(int argc, char* argv[], char* envp[]) {
int main(void) {
    signal(SIGCHLD, child);

    printf("TESTING 123\n");

    pid_t pid = fork();

    if (!pid)
        execve("/sbin/sh.elf", NULL, NULL);


    for (;;) {} // init should never ever exit

    return 0;
}
