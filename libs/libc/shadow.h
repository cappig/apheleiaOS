#pragma once

struct spwd {
    char *sp_namp;
    char *sp_pwdp;
    long sp_lstchg;
    long sp_min;
    long sp_max;
    long sp_warn;
    long sp_inact;
    long sp_expire;
    unsigned long sp_flag;
};

#ifdef _APHELEIA_SOURCE
typedef struct spwd shadow_t;
#endif

#ifndef _KERNEL
#include <libc_usr/shadow.h>
#endif
