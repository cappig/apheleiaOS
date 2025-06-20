#include "unistd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "errno.h"
#include "libc_usr/stdio.h"
#include "libc_usr/stdlib.h"
#include "stdarg.h"
#include "x86/sys.h"


pid_t fork(void) {
    return __SYSCALL_ERRNO(syscall0(SYS_FORK));
}


ssize_t read(int fd, void* buf, size_t count) {
    return __SYSCALL_ERRNO(syscall3(SYS_READ, fd, (u64)buf, count));
}

ssize_t pread(int fd, void* buf, size_t count, off_t off) {
    return __SYSCALL_ERRNO(syscall4(SYS_PREAD, fd, (u64)buf, count, off));
}


ssize_t write(int fd, const void* buf, size_t count) {
    return __SYSCALL_ERRNO(syscall3(SYS_WRITE, fd, (u64)buf, count));
}

ssize_t pwrite(int fd, const void* buf, size_t count, off_t off) {
    return __SYSCALL_ERRNO(syscall4(SYS_PWRITE, fd, (u64)buf, count, off));
}


off_t lseek(int fd, off_t offset, int whence) {
    return __SYSCALL_ERRNO(syscall3(SYS_SEEK, fd, offset, whence));
}


int close(int fd) {
    return __SYSCALL_ERRNO(syscall1(SYS_CLOSE, fd));
}


int access(const char* pathname, int mode) {
    return __SYSCALL_ERRNO(syscall2(SYS_ACCESS, (u64)pathname, mode));
}


pid_t getpid(void) {
    return syscall0(SYS_GETPID);
}

pid_t getppid(void) {
    return syscall0(SYS_GETPPID);
}


int setpgid(pid_t pid, pid_t pgrp) {
    return __SYSCALL_ERRNO(syscall2(SYS_SETPGID, pid, pgrp));
}

pid_t getpgid(pid_t pid) {
    return __SYSCALL_ERRNO(syscall1(SYS_GETPGID, pid));
}

pid_t setsid(void) {
    return __SYSCALL_ERRNO(syscall0(SYS_SETSID));
}


void _exit(int status) {
    syscall1(SYS_EXIT, status);
}

extern void __handle_atexit(void);

void exit(int status) {
    __handle_atexit();
    _exit(status);
}


int execve(const char* path, char* const argv[], char* const envp[]) {
    return __SYSCALL_ERRNO(syscall3(SYS_EXECVE, (u64)path, (u64)argv, (u64)envp));
}

static char* _find_in_path(const char* file) {
    if (strchr(file, '/'))
        return strdup(file); // if file contains '/' return as-is

    char* path = getenv("PATH");

    if (!path)
        return NULL;

    char* path_copy = strdup(path);

    if (!path_copy)
        return NULL;

    char* dir = strtok(path_copy, ":");

    while (dir) {
        size_t len = strlen(dir) + strlen(file) + 2;

        char* full_path = malloc(len);

        snprintf(full_path, len, "%s/%s", dir, file);

        if (!access(full_path, X_OK)) {
            free(path_copy);
            return full_path;
        }

        free(full_path);

        dir = strtok(NULL, ":");
    }

    free(path_copy);

    return NULL;
}

int execv(const char* path, char* const argv[]) {
    return execve(path, argv, environ);
}

int execvpe(const char* file, char* const argv[], char* const envp[]) {
    char* path = _find_in_path(file);

    if (!path) {
        errno = ENOENT;
        return -1;
    }

    int result = execve(path, argv, envp);

    free(path);

    return result;
}

int execvp(const char* file, char* const argv[]) {
    return execvpe(file, argv, environ);
}
