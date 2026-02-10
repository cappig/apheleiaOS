#include <arch/x86/sys.h>
#include <libc_usr/unistd.h>

#include <stdint.h>
#include <unistd.h>

ssize_t read(int fd, void* buf, size_t count) {
    return (ssize_t)syscall3(SYS_READ, (uintptr_t)fd, (uintptr_t)buf, (uintptr_t)count);
}

ssize_t write(int fd, const void* buf, size_t count) {
    return (ssize_t)syscall3(SYS_WRITE, (uintptr_t)fd, (uintptr_t)buf, (uintptr_t)count);
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

void _exit(int status) {
    syscall1(SYS_EXIT, (uintptr_t)status);

    for (;;)
        ;
}
