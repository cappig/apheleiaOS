#pragma once

#include <stddef.h>
#include <sys/proc.h>
#include <sys/types.h>

ssize_t read(int fd, void* buf, size_t count);
ssize_t write(int fd, const void* buf, size_t count);
ssize_t pread(int fd, void* buf, size_t count, off_t offset);
ssize_t pwrite(int fd, const void* buf, size_t count, off_t offset);
int open(const char* path, int flags, mode_t mode);
int close(int fd);
int mkdir(const char* path, mode_t mode);
int access(const char* path, int mode);
off_t lseek(int fd, off_t offset, int whence);
unsigned int sleep(unsigned int seconds);
int chdir(const char* path);
char* getcwd(char* buf, size_t size);
int link(const char* oldpath, const char* newpath);
int unlink(const char* path);
int rename(const char* oldpath, const char* newpath);

pid_t fork(void);
pid_t wait(pid_t pid, int* status);
pid_t waitpid(pid_t pid, int* status, int options);
int execve(const char* path, char* const argv[], char* const envp[]);

pid_t getpid(void);
pid_t getppid(void);
pid_t getpgid(pid_t pid);
int setpgid(pid_t pid, pid_t pgid);
pid_t setsid(void);
uid_t getuid(void);
gid_t getgid(void);
int setuid(uid_t uid);
int setgid(gid_t gid);

void _exit(int status);
