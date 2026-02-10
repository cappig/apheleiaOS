#include <arch/sys.h>
#include <libc_usr/unistd.h>

#include <stdint.h>
#include <unistd.h>

ssize_t read(int fd, void* buf, size_t count) {
    return (ssize_t)syscall3(SYS_READ, (uintptr_t)fd, (uintptr_t)buf, (uintptr_t)count);
}

ssize_t write(int fd, const void* buf, size_t count) {
    return (ssize_t)syscall3(SYS_WRITE, (uintptr_t)fd, (uintptr_t)buf, (uintptr_t)count);
}

int open(const char* path, int flags, mode_t mode) {
    return (int)syscall3(SYS_OPEN, (uintptr_t)path, (uintptr_t)flags, (uintptr_t)mode);
}

int close(int fd) {
    return (int)syscall1(SYS_CLOSE, (uintptr_t)fd);
}

off_t lseek(int fd, off_t offset, int whence) {
    return (off_t)syscall3(SYS_SEEK, (uintptr_t)fd, (uintptr_t)offset, (uintptr_t)whence);
}

unsigned int sleep(unsigned int seconds) {
    syscall1(SYS_SLEEP, (uintptr_t)seconds);
    return 0;
}

int chdir(const char* path) {
    return (int)syscall1(SYS_CHDIR, (uintptr_t)path);
}

char* getcwd(char* buf, size_t size) {
    if (!buf || !size)
        return NULL;

    int ret = (int)syscall2(SYS_GETCWD, (uintptr_t)buf, (uintptr_t)size);
    return ret == 0 ? buf : NULL;
}

int link(const char* oldpath, const char* newpath) {
    return (int)syscall2(SYS_LINK, (uintptr_t)oldpath, (uintptr_t)newpath);
}

int unlink(const char* path) {
    return (int)syscall1(SYS_UNLINK, (uintptr_t)path);
}

int rename(const char* oldpath, const char* newpath) {
    return (int)syscall2(SYS_RENAME, (uintptr_t)oldpath, (uintptr_t)newpath);
}

pid_t fork(void) {
    return (pid_t)syscall0(SYS_FORK);
}

pid_t wait(pid_t pid, int* status) {
    return (pid_t)syscall2(SYS_WAIT, (uintptr_t)pid, (uintptr_t)status);
}

int execve(const char* path, char* const argv[], char* const envp[]) {
    return (int)syscall3(SYS_EXECVE, (uintptr_t)path, (uintptr_t)argv, (uintptr_t)envp);
}

pid_t getpid(void) {
    return (pid_t)syscall0(SYS_GETPID);
}

pid_t getppid(void) {
    return (pid_t)syscall0(SYS_GETPPID);
}

uid_t getuid(void) {
    return (uid_t)syscall0(SYS_GETUID);
}

gid_t getgid(void) {
    return (gid_t)syscall0(SYS_GETGID);
}

int setuid(uid_t uid) {
    return (int)syscall1(SYS_SETUID, (uintptr_t)uid);
}

int setgid(gid_t gid) {
    return (int)syscall1(SYS_SETGID, (uintptr_t)gid);
}

void _exit(int status) {
    syscall1(SYS_EXIT, (uintptr_t)status);

    for (;;)
        ;
}
