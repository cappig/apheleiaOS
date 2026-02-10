#pragma once

#include <stddef.h>
#include <sys/types.h>

ssize_t read(int fd, void* buf, size_t count);
ssize_t write(int fd, const void* buf, size_t count);
int open(const char* path, int flags, mode_t mode);
int close(int fd);
off_t lseek(int fd, off_t offset, int whence);
unsigned int sleep(unsigned int seconds);
int chdir(const char* path);
char* getcwd(char* buf, size_t size);
int link(const char* oldpath, const char* newpath);
int unlink(const char* path);
int rename(const char* oldpath, const char* newpath);

pid_t fork(void);
pid_t wait(pid_t pid, int* status);
int execve(const char* path, char* const argv[], char* const envp[]);

pid_t getpid(void);
pid_t getppid(void);

void _exit(int status);
