#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_INPUT 1024
#define MAX_ARGS  64


void sigint_handler(int sig) {
    (void)sig;
    printf("sigint\n");
}

void sigchild_handler(int sig) {
    (void)sig;

    for (;;) {
        int status = 0;
        pid_t pid = waitpid(-1, &status, WNOHANG);

        if (pid <= 0)
            break;

        printf(">> child %zd exited with code: %d\n", pid, status);
    }
}


int read_line(char* buffer) {
    if (!fgets(buffer, MAX_INPUT, stdin))
        return 1;

    buffer[strcspn(buffer, "\n")] = 0;

    if (!strlen(buffer))
        return 1;

    return 0;
}

int tokenize_line(char* line, char** args) {
    int argc = 0;

    char* saveptr;
    char* token = strtok_r(line, " \t\r\n", &saveptr);

    while (token && argc < MAX_ARGS - 1) {
        args[argc++] = token;
        token = strtok_r(NULL, " \t\r\n", &saveptr);
    }

    args[argc] = NULL;
    return argc;
}


static int _exit_builtin(int argc, char** argv) {
    printf("\n...terminating ash\n");

    if (argc > 1)
        exit(atoi(argv[1]));
    else
        exit(0);

    return -1;
}

int handle_builtin(int argc, char** argv) {
    if (!strcmp(argv[0], "exit")) {
        _exit_builtin(argc, argv);
        return 1;
    }

    return 0;
}

int launch_program(int argc, char** argv) {
    if (handle_builtin(argc, argv))
        return 1;

    return 0;
}


int main() {
    signal(SIGINT, sigint_handler);

    pid_t pid = getpgid(0);
    ioctl(STDOUT_FILENO, TIOCSPGRP, &pid);

    char* argv[MAX_ARGS];
    char input[MAX_INPUT];

    for (;;) {
        printf("ash$ ");
        fflush(stdout);

        if (read_line(input))
            continue;

        int argc = tokenize_line(input, argv);

        printf("argc=%d\n", argc);

        launch_program(argc, argv);
    }

    return 0;
}
