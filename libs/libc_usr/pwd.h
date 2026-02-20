#pragma once

#include <stddef.h>
#include <pwd.h>

struct passwd *getpwnam(const char *name);
struct passwd *getpwuid(uid_t uid);

int getpwnam_r(
    const char *name,
    struct passwd *pwd,
    char *buf,
    size_t buflen,
    struct passwd **result
);
int getpwuid_r(uid_t uid, struct passwd *pwd, char *buf, size_t buflen, struct passwd **result);
