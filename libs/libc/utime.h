#pragma once

#include <sys/types.h>
#include <time.h>

struct utimbuf {
    time_t actime;
    time_t modtime;
};

#ifndef _KERNEL
int utime(const char *filename, const struct utimbuf *times);
#endif
