#include <errno.h>
#include <libc_usr/pwd.h>
#include <parse/textdb.h>
#include <stdlib.h>
#include <string.h>
#include <user/kv.h>

#define PASSWD_PATH "/etc/passwd"

typedef struct {
    const char *name;
    uid_t uid;
    bool by_name;
    struct passwd *pwd;
    char *buf;
    size_t buflen;
    int status;
    bool found;
} passwd_lookup_t;

static int parse_line(const char *line, struct passwd *pwd, char *buf, size_t buflen) {
    char pw_name[64] = { 0 };
    char pw_passwd[128] = { 0 };
    char uid_text[32] = { 0 };
    char gid_text[32] = { 0 };
    char pw_gecos[128] = { 0 };
    char pw_dir[128] = { 0 };
    char pw_shell[128] = { 0 };

    const char *cursor = textdb_next_field(line, pw_name, sizeof(pw_name));
    cursor = textdb_next_field(cursor, pw_passwd, sizeof(pw_passwd));
    cursor = textdb_next_field(cursor, uid_text, sizeof(uid_text));
    cursor = textdb_next_field(cursor, gid_text, sizeof(gid_text));
    cursor = textdb_next_field(cursor, pw_gecos, sizeof(pw_gecos));
    cursor = textdb_next_field(cursor, pw_dir, sizeof(pw_dir));
    textdb_next_field(cursor, pw_shell, sizeof(pw_shell));

    char *uid_end = NULL;
    char *gid_end = NULL;
    long uid = strtol(uid_text, &uid_end, 10);
    long gid = strtol(gid_text, &gid_end, 10);

    if (uid_end == uid_text || *uid_end || uid < 0 || gid_end == gid_text || *gid_end || gid < 0) {
        return EINVAL;
    }

    memset(pwd, 0, sizeof(*pwd));
    pwd->pw_uid = (uid_t)uid;
    pwd->pw_gid = (gid_t)gid;

    char *dst = buf;
    size_t left = buflen;

    int status = textdb_copy_field(&dst, &left, &pwd->pw_name, pw_name);
    if (!status) {
        status = textdb_copy_field(&dst, &left, &pwd->pw_passwd, pw_passwd);
    }
    if (!status) {
        status = textdb_copy_field(&dst, &left, &pwd->pw_gecos, pw_gecos);
    }
    if (!status) {
        status = textdb_copy_field(&dst, &left, &pwd->pw_dir, pw_dir);
    }
    if (!status) {
        status = textdb_copy_field(&dst, &left, &pwd->pw_shell, pw_shell);
    }

    return status;
}

static bool match_entry(const char *line, void *ctx) {
    passwd_lookup_t *lookup = ctx;
    struct passwd parsed = { 0 };

    int status = parse_line(line, &parsed, lookup->buf, lookup->buflen);
    if (status == EINVAL) {
        return false;
    }
    if (status) {
        lookup->status = status;
        return true;
    }

    bool hit = lookup->by_name ? (lookup->name && !strcmp(parsed.pw_name, lookup->name))
                               : (parsed.pw_uid == lookup->uid);
    if (!hit) {
        return false;
    }

    *lookup->pwd = parsed;
    lookup->found = true;
    return true;
}

static int find_passwd(passwd_lookup_t *lookup) {
    char text[4096];

    if (kv_read_file(PASSWD_PATH, text, sizeof(text)) > 0) {
        textdb_scan(text, match_entry, lookup);
    }

    return lookup->status;
}

int getpwnam_r(const char *name, struct passwd *pwd, char *buf, size_t buflen, struct passwd **result) {
    if (!name || !pwd || !buf || !buflen || !result) {
        return EINVAL;
    }

    passwd_lookup_t lookup = {
        .name = name,
        .by_name = true,
        .pwd = pwd,
        .buf = buf,
        .buflen = buflen,
    };

    int status = find_passwd(&lookup);
    *result = lookup.found ? pwd : NULL;

    return status;
}

int getpwuid_r(uid_t uid, struct passwd *pwd, char *buf, size_t buflen, struct passwd **result) {
    if (!pwd || !buf || !buflen || !result) {
        return EINVAL;
    }

    passwd_lookup_t lookup = {
        .uid = uid,
        .pwd = pwd,
        .buf = buf,
        .buflen = buflen,
    };

    int status = find_passwd(&lookup);
    *result = lookup.found ? pwd : NULL;

    return status;
}

struct passwd *getpwnam(const char *name) {
    static struct passwd pwd;
    static char buf[512];

    struct passwd *result = NULL;
    int status = getpwnam_r(name, &pwd, buf, sizeof(buf), &result);

    if (status || !result) {
        errno = status ? status : ENOENT;
        return NULL;
    }

    return result;
}

struct passwd *getpwuid(uid_t uid) {
    static struct passwd pwd;
    static char buf[512];

    struct passwd *result = NULL;
    int status = getpwuid_r(uid, &pwd, buf, sizeof(buf), &result);

    if (status || !result) {
        errno = status ? status : ENOENT;
        return NULL;
    }

    return result;
}
