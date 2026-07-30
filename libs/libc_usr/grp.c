#include <errno.h>
#include <libc_usr/grp.h>
#include <parse/textdb.h>
#include <stdlib.h>
#include <string.h>
#include <user/kv.h>

#define GROUP_PATH "/etc/group"

typedef struct {
    gid_t gid;
    struct group *grp;
    char *buf;
    size_t buflen;
    int status;
    bool found;
} group_lookup_t;

static int parse_line(const char *line, struct group *grp, char *buf, size_t buflen) {
    char gr_name[64] = { 0 };
    char gr_passwd[64] = { 0 };
    char gid_text[32] = { 0 };

    const char *cursor = textdb_next_field(line, gr_name, sizeof(gr_name));
    cursor = textdb_next_field(cursor, gr_passwd, sizeof(gr_passwd));
    textdb_next_field(cursor, gid_text, sizeof(gid_text));

    char *end = NULL;
    long gid = strtol(gid_text, &end, 10);
    if (end == gid_text || *end != '\0' || gid < 0) {
        return EINVAL;
    }

    memset(grp, 0, sizeof(*grp));
    grp->gr_gid = (gid_t)gid;

    char *dst = buf;
    size_t left = buflen;

    int status = textdb_copy_field(&dst, &left, &grp->gr_name, gr_name);
    if (!status) {
        status = textdb_copy_field(&dst, &left, &grp->gr_passwd, gr_passwd);
    }

    return status;
}

static bool match_gid(const char *line, void *ctx) {
    group_lookup_t *lookup = ctx;
    struct group parsed = { 0 };

    int status = parse_line(line, &parsed, lookup->buf, lookup->buflen);
    if (status == EINVAL) {
        return false;
    }
    if (status) {
        lookup->status = status;
        return true;
    }
    if (parsed.gr_gid != lookup->gid) {
        return false;
    }

    *lookup->grp = parsed;
    lookup->found = true;
    return true;
}

int getgrgid_r(gid_t gid, struct group *grp, char *buf, size_t buflen, struct group **result) {
    if (!grp || !buf || !buflen || !result) {
        return EINVAL;
    }

    group_lookup_t lookup = {
        .gid = gid,
        .grp = grp,
        .buf = buf,
        .buflen = buflen,
    };

    char text[4096];
    if (kv_read_file(GROUP_PATH, text, sizeof(text)) > 0) {
        textdb_scan(text, match_gid, &lookup);
    }

    *result = lookup.found ? grp : NULL;

    return lookup.status;
}

struct group *getgrgid(gid_t gid) {
    static struct group grp;
    static char buf[256];

    struct group *result = NULL;
    int status = getgrgid_r(gid, &grp, buf, sizeof(buf), &result);

    if (status || !result) {
        errno = status ? status : ENOENT;
        return NULL;
    }

    return result;
}
