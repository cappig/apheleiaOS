#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_INPUT 1024

void sigint_handler(int sig) {
    (void)sig;
    printf("sigint\n");
}

int main() {
    signal(SIGINT, sigint_handler);

    pid_t pid = getpid();
    ioctl(STDOUT_FILENO, TIOCSPGRP, &pid);

    char input[MAX_INPUT];

    for (;;) {
        printf("ash$ ");
        fflush(stdout);

        if (!fgets(input, MAX_INPUT, stdin))
            break;

        input[strcspn(input, "\n")] = 0;

        if (!strlen(input))
            continue;

        printf("'%s'\n", input);

        if (!strcmp(input, "exit"))
            break;
    }

    printf("\n...terminating ash\n");
    return 0;
}
