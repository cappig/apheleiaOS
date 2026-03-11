#pragma once

#include <sys/stat.h>
#include <stddef.h>
#include <sys/types.h>

extern char **environ;

ssize_t read(int fd, void *buf, size_t count);
ssize_t write(int fd, const void *buf, size_t count);
ssize_t pread(int fd, void *buf, size_t count, off_t offset);
ssize_t pwrite(int fd, const void *buf, size_t count, off_t offset);
int open(const char *path, int flags, ...);
int close(int fd);
int pipe(int pipefd[2]);
int dup(int oldfd);
int dup2(int oldfd, int newfd);
int mkdir(const char *path, mode_t mode);
int rmdir(const char *path);
int access(const char *path, int mode);
off_t lseek(int fd, off_t offset, int whence);
mode_t umask(mode_t mask);
unsigned int sleep(unsigned int seconds);
int usleep(useconds_t usec);
int chdir(const char *path);
char *getcwd(char *buf, size_t size);
int isatty(int fd);
int link(const char *oldpath, const char *newpath);
int symlink(const char *target, const char *linkpath);
ssize_t readlink(const char *path, char *buf, size_t bufsiz);
int unlink(const char *path);
int rename(const char *oldpath, const char *newpath);
int openat(int dirfd, const char *path, int flags, ...);
int fstatat(int dirfd, const char *path, struct stat *st, int flags);
int faccessat(int dirfd, const char *path, int mode, int flags);
int mkdirat(int dirfd, const char *path, mode_t mode);
int unlinkat(int dirfd, const char *path, int flags);
int renameat(
    int olddirfd,
    const char *oldpath,
    int newdirfd,
    const char *newpath
);
int linkat(
    int olddirfd,
    const char *oldpath,
    int newdirfd,
    const char *newpath,
    int flags
);
int fchmod(int fd, mode_t mode);
int fchown(int fd, uid_t uid, gid_t gid);
int truncate(const char *path, off_t length);
int ftruncate(int fd, off_t length);
int fsync(int fd);
int fdatasync(int fd);
int mount(
    const char *source,
    const char *target,
    const char *filesystemtype,
    unsigned long flags
);
int umount(const char *target, unsigned long flags);

pid_t fork(void);
pid_t wait(int *status);
pid_t waitpid(pid_t pid, int *status, int options);
int execve(const char *path, char *const argv[], char *const envp[]);
int execvp(const char *file, char *const argv[]);

pid_t getpid(void);
pid_t getppid(void);
pid_t getpgid(pid_t pid);
int setpgid(pid_t pid, pid_t pgid);
pid_t setsid(void);
uid_t getuid(void);
gid_t getgid(void);
int setuid(uid_t uid);
int setgid(gid_t gid);
int getgroups(int size, gid_t list[]);
int setgroups(size_t size, const gid_t list[]);
long sysconf(int name);

void _exit(int status) __attribute__((noreturn));
