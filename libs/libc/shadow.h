#pragma once

#include <sys/types.h>

typedef struct shadow {
    char sp_name[32];
    char sp_pwd[64];
} shadow_t;

#ifndef _KERNEL
#include <libc_usr/shadow.h>
#endif
