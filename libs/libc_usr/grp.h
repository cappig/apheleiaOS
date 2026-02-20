#pragma once

#include <stddef.h>
#include <grp.h>

struct group *getgrgid(gid_t gid);
int getgrgid_r(
    gid_t gid,
    struct group *grp,
    char *buf,
    size_t buflen,
    struct group **result
);
