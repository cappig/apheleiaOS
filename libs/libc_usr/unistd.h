#pragma once

#include <stddef.h>
#include <sys/types.h>

#define ATEXIT_MAX 32

extern char** environ;


pid_t fork(void);

ssize_t read(int fd, void* buf, size_t count);
ssize_t pread(int fd, void* buf, size_t count, off_t off);

ssize_t write(int fd, const void* buf, size_t count);
ssize_t pwrite(int fd, const void* buf, size_t count, off_t off);

off_t lseek(int fd, off_t offset, int whence);

int close(int fd);

int access(const char* pathname, int mode);

pid_t getpid(void);
pid_t getppid(void);

int setpgid(pid_t pid, pid_t pgrp);
pid_t getpgid(pid_t pid);

pid_t setsid(void);

void _exit(int status);
void exit(int status);

int execve(const char* path, char* const argv[], char* const envp[]);
int execv(const char* path, char* const argv[]);
int execvpe(const char* file, char* const argv[], char* const envp[]);
int execvp(const char* file, char* const argv[]);
// TODO: missing a bunch of functions
