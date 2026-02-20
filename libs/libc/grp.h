#pragma once

#include <sys/types.h>

struct group {
    char *gr_name;
    char *gr_passwd;
    gid_t gr_gid;
    char **gr_mem;
};

#ifdef _APHELEIA_SOURCE
typedef struct group group_t;
#endif

#ifndef _KERNEL
#include <libc_usr/grp.h>
#endif
