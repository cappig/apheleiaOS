#pragma once

// FIXME: this should be arch generic
#include <x86/sys.h>

#define SYS_EXIT      0
#define SYS_READ      1
#define SYS_PREAD     2
#define SYS_WRITE     3
#define SYS_PWRITE    4
#define SYS_SEEK      5
#define SYS_OPEN      6
#define SYS_CLOSE     7
#define SYS_MKDIR     8
#define SYS_ACCESS    9
#define SYS_IOCTL     10
#define SYS_SIGNAL    11
#define SYS_SIGRETURN 12
#define SYS_KILL      13
#define SYS_WAIT      14
#define SYS_GETPID    15
#define SYS_GETPPID   16
#define SYS_SETPGID   17
#define SYS_GETPGID   18
#define SYS_SETSID    19
#define SYS_FORK      20
#define SYS_EXECVE    21
#define SYS_SLEEP     22
#define SYS_MOUNT     23
#define SYS_UNMOUNT   24
#define SYS_MMAP      25

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#define R_OK 4
#define W_OK 2
#define X_OK 1

#ifndef _KERNEL
#include <libc_usr/unistd.h>
#endif
