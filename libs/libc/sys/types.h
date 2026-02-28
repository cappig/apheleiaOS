#pragma once

#include <posix.h>

#include <stddef.h>
#include <stdint.h>

typedef ptrdiff_t ssize_t;

typedef int pid_t;
typedef int tid_t;
typedef unsigned int uid_t;
typedef unsigned int gid_t;
typedef unsigned int useconds_t;
typedef long suseconds_t;

typedef long long off_t;
typedef long long blkcnt_t;
typedef long blksize_t;
typedef long clock_t;
typedef int id_t;
typedef unsigned long fsblkcnt_t;
typedef unsigned long fsfilcnt_t;

typedef uint32_t mode_t;

typedef uint32_t dev_t;
typedef uint64_t ino_t;
typedef uint32_t nlink_t;
