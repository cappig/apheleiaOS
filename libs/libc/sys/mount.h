#pragma once

#include <sys/types.h>

#define MS_RDONLY   (1ULL << 0)
#define MS_NOSUID   (1ULL << 1)
#define MS_NODEV    (1ULL << 2)
#define MS_NOEXEC   (1ULL << 3)
#define MS_SYNCHRONOUS (1ULL << 4)
#define MS_REMOUNT  (1ULL << 5)
#define MS_BIND     (1ULL << 6)

#ifndef _KERNEL
int mount(
    const char *source,
    const char *target,
    const char *filesystemtype,
    unsigned long flags
);
int umount(const char *target, unsigned long flags);
#endif
