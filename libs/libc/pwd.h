#pragma once

#include <sys/types.h>

typedef struct passwd {
    char pw_name[32];
    char pw_passwd[64];
    uid_t pw_uid;
    gid_t pw_gid;
    char pw_gecos[64];
    char pw_dir[64];
    char pw_shell[64];
} passwd_t;

#ifndef _KERNEL
#include <libc_usr/pwd.h>
#endif
