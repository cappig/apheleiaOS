#include <errno.h>
#include <libc_usr/grp.h>
#include <parse/textdb.h>
#include <stdlib.h>
#include <string.h>
#include <user/kv.h>

#define GROUP_PATH "/etc/group"

static int copy_field(char **cursor, size_t *left, char **out, const char *src) {
    size_t n = strlen(src) + 1;
    if (!cursor || !left || !out || !src || n > *left) {
        return ERANGE;
    }

    memcpy(*cursor, src, n);
    *out = *cursor;
    *cursor += n;
    *left -= n;
    return 0;
}

static int parse_group_line(const char *line, struct group *grp, char *buf, size_t buflen) {
    if (!line || !grp || !buf || !buflen) {
        return EINVAL;
    }

    char gr_name[64] = {0};
    char gr_passwd[64] = {0};
    char gid_buf[32] = {0};

    const char *cursor = line;
    cursor = textdb_next_field(cursor, gr_name, sizeof(gr_name));
    cursor = textdb_next_field(cursor, gr_passwd, sizeof(gr_passwd));
    cursor = textdb_next_field(cursor, gid_buf, sizeof(gid_buf));
    (void)cursor;

    char *end = NULL;
    long gid = strtol(gid_buf, &end, 10);
    if (end == gid_buf || *end != '\0' || gid < 0) {
        return EINVAL;
    }

    memset(grp, 0, sizeof(*grp));
    grp->gr_gid = (gid_t)gid;
    grp->gr_mem = NULL;

    char *dst = buf;
    size_t left = buflen;
    int rc = copy_field(&dst, &left, &grp->gr_name, gr_name);
    if (!rc) {
        rc = copy_field(&dst, &left, &grp->gr_passwd, gr_passwd);
    }

    return rc;
}

int getgrgid_r(gid_t gid, struct group *grp, char *buf, size_t buflen, struct group **result) {
    if (!grp || !buf || !buflen || !result) {
        return EINVAL;
    }

    char file_buf[4096];
    ssize_t len = kv_read_file(GROUP_PATH, file_buf, sizeof(file_buf));
    if (len <= 0) {
        return ENOENT;
    }

    const char *line = file_buf;
    while (*line) {
        const char *next = strchr(line, '\n');
        size_t line_len = next ? (size_t)(next - line) : strlen(line);

        if (line_len) {
            char tmp[256];
            if (line_len >= sizeof(tmp)) {
                return ERANGE;
            }

            memcpy(tmp, line, line_len);
            tmp[line_len] = '\0';

            struct group parsed = {0};
            int rc = parse_group_line(tmp, &parsed, buf, buflen);
            if (rc && rc != EINVAL) {
                return rc;
            }

            if (!rc && parsed.gr_gid == gid) {
                *grp = parsed;
                *result = grp;
                return 0;
            }
        }

        if (!next) {
            break;
        }
        line = next + 1;
    }

    *result = NULL;
    return 0;
}

struct group *getgrgid(gid_t gid) {
    static struct group grp;
    static char buf[256];
    struct group *result = NULL;
    int rc = getgrgid_r(gid, &grp, buf, sizeof(buf), &result);
    if (rc || !result) {
        errno = rc ? rc : ENOENT;
        return NULL;
    }

    return result;
}
