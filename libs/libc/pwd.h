#pragma once

#include <sys/types.h>

struct passwd {
    char* pw_name;
    char* pw_passwd;
    uid_t pw_uid;
    gid_t pw_gid;
    char* pw_gecos;
    char* pw_dir;
    char* pw_shell;
};


#ifndef _KERNEL
#include <libc_usr/pwd.h>
#endif
