#include "account.h"

#include <grp.h>
#include <pwd.h>
#include <stdio.h>

const char *account_uid_name(uid_t uid, char *buf, size_t len) {
    if (!buf || !len) {
        return "";
    }

    struct passwd *pwd = getpwuid(uid);
    if (pwd && pwd->pw_name && pwd->pw_name[0]) {
        snprintf(buf, len, "%s", pwd->pw_name);
        return buf;
    }

    snprintf(buf, len, "%llu", (unsigned long long)uid);

    return buf;
}

const char *account_gid_name(gid_t gid, char *buf, size_t len) {
    if (!buf || !len) {
        return "";
    }

    struct group *grp = getgrgid(gid);
    if (grp && grp->gr_name && grp->gr_name[0]) {
        snprintf(buf, len, "%s", grp->gr_name);
        return buf;
    }

    snprintf(buf, len, "%llu", (unsigned long long)gid);

    return buf;
}
