#pragma once

#include <shadow.h>
#include <stddef.h>

struct spwd *getspnam(const char *name);
int getspnam_r(const char *name, struct spwd *spbuf, char *buf, size_t buflen, struct spwd **result);
