#include <errno.h>
#include <libc_usr/shadow.h>
#include <parse/textdb.h>
#include <string.h>
#include <user/kv.h>

#define SHADOW_PATH "/etc/shadow"

typedef struct {
    const char *name;
    struct spwd *spbuf;
    char *buf;
    size_t buflen;
    int status;
    bool found;
} shadow_lookup_t;

static int parse_line(const char *line, struct spwd *spbuf, char *buf, size_t buflen) {
    char name[64] = { 0 };
    char pwd[128] = { 0 };

    const char *cursor = textdb_next_field(line, name, sizeof(name));
    textdb_next_field(cursor, pwd, sizeof(pwd));

    memset(spbuf, 0, sizeof(*spbuf));
    spbuf->sp_lstchg = -1;
    spbuf->sp_min = -1;
    spbuf->sp_max = -1;
    spbuf->sp_warn = -1;
    spbuf->sp_inact = -1;
    spbuf->sp_expire = -1;

    char *dst = buf;
    size_t left = buflen;

    int status = textdb_copy_field(&dst, &left, &spbuf->sp_namp, name);
    if (!status) {
        status = textdb_copy_field(&dst, &left, &spbuf->sp_pwdp, pwd);
    }

    return status;
}

static bool match_name(const char *line, void *ctx) {
    shadow_lookup_t *lookup = ctx;
    struct spwd parsed = { 0 };

    int status = parse_line(line, &parsed, lookup->buf, lookup->buflen);
    if (status == EINVAL) {
        return false;
    }
    if (status) {
        lookup->status = status;
        return true;
    }
    if (strcmp(parsed.sp_namp, lookup->name)) {
        return false;
    }

    *lookup->spbuf = parsed;
    lookup->found = true;
    return true;
}

int getspnam_r(const char *name, struct spwd *spbuf, char *buf, size_t buflen, struct spwd **result) {
    if (!name || !spbuf || !buf || !buflen || !result) {
        return EINVAL;
    }

    shadow_lookup_t lookup = {
        .name = name,
        .spbuf = spbuf,
        .buf = buf,
        .buflen = buflen,
    };

    char text[4096];
    if (kv_read_file(SHADOW_PATH, text, sizeof(text)) > 0) {
        textdb_scan(text, match_name, &lookup);
    }

    *result = lookup.found ? spbuf : NULL;

    return lookup.status;
}

struct spwd *getspnam(const char *name) {
    static struct spwd spbuf;
    static char buf[256];

    struct spwd *result = NULL;
    int status = getspnam_r(name, &spbuf, buf, sizeof(buf), &result);

    if (status || !result) {
        errno = status ? status : ENOENT;
        return NULL;
    }

    return result;
}
