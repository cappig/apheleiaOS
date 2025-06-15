#pragma once

#include <stddef.h>
#include <sys/types.h>


int execve(const char* path, char* const argv[], char* const envp[]);

pid_t fork(void);

ssize_t read(int fd, void* buf, size_t count);
ssize_t pread(int fd, void* buf, size_t count, off_t off);

ssize_t write(int fd, const void* buf, size_t count);
ssize_t pwrite(int fd, const void* buf, size_t count, off_t off);

off_t lseek(int fd, off_t offset, int whence);

int close(int fd);

pid_t getpid(void);
pid_t getppid(void);

void _exit(int status);
