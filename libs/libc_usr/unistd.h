#pragma once

#include <stddef.h>
#include <sys/types.h>

ssize_t read(int fd, void* buf, size_t count);
ssize_t write(int fd, const void* buf, size_t count);

pid_t fork(void);
pid_t wait(pid_t pid, int* status);
int execve(const char* path, char* const argv[], char* const envp[]);

pid_t getpid(void);
pid_t getppid(void);

void _exit(int status);
