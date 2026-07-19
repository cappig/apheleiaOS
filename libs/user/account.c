#include "account.h"

#include <crypt.h>
#include <ctype.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <shadow.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <user/kv.h>

#define GROUP_PATH "/etc/group"

static bool secure_equal(const char *left, const char *right) {
    if (!left || !right) {
        return false;
    }

    size_t left_len = strlen(left);
    size_t right_len = strlen(right);
    size_t count = left_len > right_len ? left_len : right_len;
    unsigned char diff = left_len != right_len;

    for (size_t i = 0; i < count; i++) {
        unsigned char a = i < left_len ? (unsigned char)left[i] : 0;
        unsigned char b = i < right_len ? (unsigned char)right[i] : 0;
        diff |= (unsigned char)(a ^ b);
    }

    return diff == 0;
}

static char *trim(char *text) {
    while (*text && isspace((unsigned char)*text)) {
        text++;
    }

    char *end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) {
        end--;
    }

    *end = '\0';
    return text;
}

static bool has_member(const char *members, const char *name) {
    char text[256];
    size_t len = strnlen(members, sizeof(text));
    if (len >= sizeof(text)) {
        return false;
    }

    memcpy(text, members, len + 1);

    char *save = NULL;
    for (char *member = strtok_r(text, ",", &save); member; member = strtok_r(NULL, ",", &save)) {
        if (!strcmp(trim(member), name)) {
            return true;
        }
    }

    return false;
}

static void add_group(gid_t gid, gid_t *groups, size_t *count, size_t max_groups) {
    for (size_t i = 0; i < *count; i++) {
        if (groups[i] == gid) {
            return;
        }
    }

    if (*count < max_groups) {
        groups[(*count)++] = gid;
    }
}

static void parse_group(char *line, const char *name, gid_t primary_gid, gid_t *groups, size_t *count, size_t max) {
    char *save = NULL;
    (void)strtok_r(line, ":", &save);
    (void)strtok_r(NULL, ":", &save);
    char *gid_text = strtok_r(NULL, ":", &save);
    char *members = strtok_r(NULL, ":", &save);

    if (!gid_text || !members) {
        return;
    }

    char *gid_field = trim(gid_text);
    if (gid_field[0] == '-') {
        return;
    }

    char *end = NULL;
    unsigned long long value = strtoull(gid_field, &end, 10);
    gid_t gid = (gid_t)value;
    if (end == gid_field || *trim(end) || (unsigned long long)gid != value) {
        return;
    }

    if (gid != primary_gid && has_member(members, name)) {
        add_group(gid, groups, count, max);
    }
}

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

bool account_auth(const char *name, const char *password) {
    if (!name || !password) {
        return false;
    }

    struct spwd *shadow = getspnam(name);
    if (!shadow || !shadow->sp_pwdp) {
        return false;
    }

    return secure_equal(crypt(password, shadow->sp_pwdp), shadow->sp_pwdp);
}

size_t account_groups(const char *name, gid_t primary_gid, gid_t *groups, size_t max_groups) {
    if (!name || !name[0] || !groups || !max_groups) {
        return 0;
    }

    char text[4096];
    int fd = open(GROUP_PATH, O_RDONLY, 0);
    if (fd < 0) {
        return 0;
    }

    ssize_t len = kv_read_fd(fd, text, sizeof(text));
    close(fd);
    if (len <= 0) {
        return 0;
    }

    size_t count = 0;
    char *save = NULL;
    for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        line = trim(line);
        if (line[0] && line[0] != '#') {
            parse_group(line, name, primary_gid, groups, &count, max_groups);
        }
    }

    return count;
}
