#pragma once

#include <sys/types.h>

typedef struct group {
    char gr_name[32];
    gid_t gr_gid;
} group_t;

#ifndef _KERNEL
#include <libc_usr/grp.h>
#endif
