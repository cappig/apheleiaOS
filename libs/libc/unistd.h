#pragma once

// FIXME: this should be arch generic

#define SYS_EXIT      0
#define SYS_READ      1
#define SYS_WRITE     2
#define SYS_OPEN      3
#define SYS_CLOSE     4
#define SYS_PREAD     5
#define SYS_PWRITE    6
#define SYS_SEEK      7
#define SYS_MMAP      8
#define SYS_MUNMAP    9
#define SYS_IOCTL     10
#define SYS_GETDENTS  11
#define SYS_CHDIR     12
#define SYS_GETCWD    13
#define SYS_MKDIR     14
#define SYS_ACCESS    15
#define SYS_STAT      16
#define SYS_LSTAT     17
#define SYS_FSTAT     18
#define SYS_CHMOD     19
#define SYS_CHOWN     20
#define SYS_LINK      21
#define SYS_UNLINK    22
#define SYS_RENAME    23
#define SYS_FORK      24
#define SYS_EXECVE    25
#define SYS_WAIT      26
#define SYS_GETPID    27
#define SYS_GETPPID   28
#define SYS_GETUID    29
#define SYS_GETGID    30
#define SYS_SETUID    31
#define SYS_SETGID    32
#define SYS_SETPGID   33
#define SYS_GETPGID   34
#define SYS_SETSID    35
#define SYS_SLEEP     36
#define SYS_SIGNAL    37
#define SYS_SIGRETURN 38
#define SYS_KILL      39
#define SYS_GETPROCS  40
#define SYS_WAITPID   41
#define SYS_PIPE      42
#define SYS_DUP       43
#define SYS_RMDIR     44
#define SYS_UMASK     45
#define SYS_POLL      46

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#define R_OK 4
#define W_OK 2
#define X_OK 1

#ifndef _KERNEL
#include <libc_usr/unistd.h>
#endif
