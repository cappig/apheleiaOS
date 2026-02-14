#pragma once

#include <base/types.h>

#define DIRENT_NAME_MAX 64

typedef struct dirent {
    u32 d_ino;
    u32 d_type;
    char d_name[DIRENT_NAME_MAX];
} dirent_t;

int getdents(int fd, dirent_t* out);
