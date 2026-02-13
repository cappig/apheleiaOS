#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SYSTEM_MAX_ARGS 16
#define SYSTEM_MAX_CMD  256

static int split_command(char* buf, char** argv, int max) {
    if (!buf || !argv || max <= 0)
        return 0;

    int argc = 0;
    char* cursor = buf;

    while (*cursor && argc < max - 1) {
        while (*cursor && isspace((unsigned char)*cursor))
            cursor++;

        if (!*cursor)
            break;

        argv[argc++] = cursor;

        while (*cursor && !isspace((unsigned char)*cursor))
            cursor++;

        if (*cursor)
            *cursor++ = '\0';
    }

    argv[argc] = NULL;
    return argc;
}

int system(const char* command) {
    if (!command)
        return 1;

    char cmdline[SYSTEM_MAX_CMD];

    size_t len = strnlen(command, sizeof(cmdline) - 1);
    memcpy(cmdline, command, len);
    cmdline[len] = '\0';

    char* argv[SYSTEM_MAX_ARGS];
    int argc = split_command(cmdline, argv, SYSTEM_MAX_ARGS);

    if (!argc)
        return 0;

    pid_t pid = fork();

    if (!pid) {
        if (strchr(argv[0], '/')) {
            execve(argv[0], argv, environ);
        } else {
            char path[128];
            snprintf(path, sizeof(path), "/sbin/%s", argv[0]);
            execve(path, argv, environ);
        }

        _exit(127);
    }

    if (pid < 0)
        return -1;

    int status = 0;

    if (wait(pid, &status) < 0)
        return -1;

    return status;
}
