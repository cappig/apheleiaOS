#pragma once

#include <sys/types.h>
#include <sys/time.h>

#define RUSAGE_SELF     0
#define RUSAGE_CHILDREN -1

struct rusage {
    struct timeval ru_utime;
    struct timeval ru_stime;
};

#ifndef _KERNEL
int getrusage(int who, struct rusage *usage);
#endif
