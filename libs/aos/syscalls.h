#pragma once

#include <base/macros.h>
#include <base/types.h>
#include <stddef.h>

#include "x86/asm.h"

#define SYSCALL_INT 0x80

#define _SYS_ASM "int $" STR(SYSCALL_INT)

typedef ptrdiff_t ssize_t;
typedef ssize_t off_t;
typedef ssize_t pid_t;
typedef ssize_t uid_t;

typedef void (*sighandler_t)(int);

// Syscalls use int 0x80 to interrupt the kernel
// The ABI is very similar to System V:
// The syscall number is passed in %rax
// Arguments are passed in from left to right in %rdi, %rsi, %rdx, %rcx, %r8, %r9
// A single return value is placed in %rax
// TODO: implement lighter more modern interfaces (sysenter/syscall)

enum syscall_nums {
    SYS_EXIT = 0,

    SYS_READ = 1,
    SYS_PREAD = 2,
    SYS_WRITE = 3,
    SYS_PWRITE = 4,
    SYS_SEEK = 5,
    SYS_OPEN = 6,

    SYS_SIGNAL = 7,
    SYS_SIGRETURN = 8,

    SYS_KILL = 9,
    SYS_WAIT = 10,

    SYS_GETPID = 11,
    SYS_GETPPID = 12,

    SYS_FORK = 13,
    // SYS_EXECVE = 14,

    SYS_SLEEP = 15,

    SYSCALL_COUNT
};

enum std_streams {
    STDIN_FD = 0,
    STDOUT_FD = 1,
    STDERR_FD = 2,
};

enum seek_whence {
    SYS_SEEK_SET = 0, // off = x
    SYS_SEEK_CUR = 1, // off += x
    SYS_SEEK_END = 2, // off = eof + x
};

enum wait_options {
    WNOHANG = 1 << 0,
};


inline u64 syscall0(u64 num) {
    u64 ret = num;
    asm volatile(_SYS_ASM : "=a"(ret) : "a"(ret) : "memory");

    return ret;
}

inline u64 syscall1(u64 num, u64 arg1) {
    u64 ret = num;
    asm volatile(_SYS_ASM : "=a"(ret) : "a"(ret), "D"(arg1) : "memory");

    return ret;
}

inline u64 syscall2(u64 num, u64 arg1, u64 arg2) {
    u64 ret = num;
    asm volatile(_SYS_ASM : "=a"(ret) : "a"(ret), "D"(arg1), "S"(arg2) : "memory");

    return ret;
}

inline u64 syscall3(u64 num, u64 arg1, u64 arg2, u64 arg3) {
    u64 ret = num;
    asm volatile(_SYS_ASM : "=a"(ret) : "a"(ret), "D"(arg1), "S"(arg2), "d"(arg3) : "memory");

    return ret;
}

inline u64 syscall4(u64 num, u64 arg1, u64 arg2, u64 arg3, u64 arg4) {
    u64 ret = num;
    asm volatile(_SYS_ASM
                 : "=a"(ret)
                 : "a"(ret), "D"(arg1), "S"(arg2), "d"(arg3), "c"(arg4)
                 : "memory");

    return ret;
}


inline void sys_exit(int status) {
    syscall1(SYS_EXIT, status);
}


inline ssize_t sys_read(int fd, void* buffer, size_t len) {
    return syscall3(SYS_READ, fd, (u64)buffer, len);
}

inline ssize_t sys_pread(int fd, void* buffer, size_t len, off_t offset) {
    return syscall4(SYS_PREAD, fd, (u64)buffer, len, offset);
}


inline ssize_t sys_write(int fd, void* buffer, size_t len) {
    return syscall3(SYS_WRITE, fd, (u64)buffer, len);
}

inline ssize_t sys_pwrite(int fd, void* buffer, size_t len, off_t offset) {
    return syscall4(SYS_PWRITE, fd, (u64)buffer, len, offset);
}

inline off_t sys_seek(int fd, off_t offset, int whence) {
    return syscall3(SYS_SEEK, fd, offset, whence);
}


inline sighandler_t sys_signal(int signum, sighandler_t handler) {
    return (sighandler_t)syscall2(SYS_SIGNAL, signum, (u64)handler);
}


inline int sys_kill(pid_t pid, int signum) {
    return syscall2(SYS_KILL, pid, signum);
}

inline int sys_wait(pid_t pid, int* status, int options) {
    return syscall3(SYS_WAIT, pid, (u64)status, options);
}


inline pid_t sys_fork(void) {
    return syscall0(SYS_FORK);
}

// inline int sys_execve(char const* path, char const* argv[], char const* envp[]) {
//     return syscall3(SYS_EXECVE, (u64)path, (u64)argv, (u64)envp);
// }


inline pid_t sys_getpid(void) {
    return syscall0(SYS_GETPID);
}

inline pid_t sys_getppid(void) {
    return syscall0(SYS_GETPPID);
}

inline pid_t sys_sleep(size_t milis) {
    return syscall1(SYS_SLEEP, milis);
}
