#pragma once

#include <stddef.h>
#include <shadow.h>

struct spwd *getspnam(const char *name);
int getspnam_r(
    const char *name,
    struct spwd *spbuf,
    char *buf,
    size_t buflen,
    struct spwd **result
);
