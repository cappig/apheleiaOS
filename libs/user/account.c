#include "account.h"

#include <grp.h>
#include <pwd.h>
#include <stdio.h>

const char *account_uid_name(uid_t uid, char *buf, size_t len) {
    if (!buf || !len) {
        return "";
    }

    passwd_t pwd = {0};

    if (!getpwuid(uid, &pwd) && pwd.pw_name[0]) {
        snprintf(buf, len, "%s", pwd.pw_name);
        return buf;
    }

    snprintf(buf, len, "%llu", (unsigned long long)uid);

    return buf;
}

const char *account_gid_name(gid_t gid, char *buf, size_t len) {
    if (!buf || !len) {
        return "";
    }

    group_t grp = {0};

    if (!getgrgid(gid, &grp) && grp.gr_name[0]) {
        snprintf(buf, len, "%s", grp.gr_name);
        return buf;
    }

    snprintf(buf, len, "%llu", (unsigned long long)gid);

    return buf;
}
