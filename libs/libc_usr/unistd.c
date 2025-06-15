#include <sys/types.h>
#include <unistd.h>

#include "x86/sys.h"


int execve(const char* path, char* const argv[], char* const envp[]) {
    return syscall3(SYS_EXECVE, (u64)path, (u64)argv, (u64)envp);
}


pid_t fork(void) {
    return syscall0(SYS_FORK);
}


ssize_t read(int fd, void* buf, size_t count) {
    return syscall3(SYS_READ, fd, (u64)buf, count);
}

ssize_t pread(int fd, void* buf, size_t count, off_t off) {
    return syscall4(SYS_PREAD, fd, (u64)buf, count, off);
}


ssize_t write(int fd, const void* buf, size_t count) {
    return syscall3(SYS_WRITE, fd, (u64)buf, count);
}

ssize_t pwrite(int fd, const void* buf, size_t count, off_t off) {
    return syscall4(SYS_PWRITE, fd, (u64)buf, count, off);
}


off_t lseek(int fd, off_t offset, int whence) {
    return syscall3(SYS_SEEK, fd, offset, whence);
}


int close(int fd) {
    return syscall1(SYS_CLOSE, fd);
}


pid_t getpid(void) {
    return syscall0(SYS_GETPID);
}

pid_t getppid(void) {
    return syscall0(SYS_GETPPID);
}


void _exit(int status) {
    syscall1(SYS_EXIT, status);
}
