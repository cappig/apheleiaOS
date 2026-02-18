#include <libc_usr/pwd.h>
#include <parse/textdb.h>
#include <stdlib.h>
#include <string.h>
#include <user/kv.h>

#define PASSWD_PATH "/etc/passwd"

static bool parse_passwd_line(
    const char *line,
    const char *match_name,
    uid_t match_uid,
    bool by_name,
    passwd_t *out
) {
    passwd_t entry = {0};
    char uid_buf[32] = {0};
    char gid_buf[32] = {0};

    const char *cursor = line;

    cursor = textdb_next_field(cursor, entry.pw_name, sizeof(entry.pw_name));
    cursor = textdb_next_field(cursor, entry.pw_passwd, sizeof(entry.pw_passwd));
    cursor = textdb_next_field(cursor, uid_buf, sizeof(uid_buf));
    cursor = textdb_next_field(cursor, gid_buf, sizeof(gid_buf));
    cursor = textdb_next_field(cursor, entry.pw_gecos, sizeof(entry.pw_gecos));
    cursor = textdb_next_field(cursor, entry.pw_dir, sizeof(entry.pw_dir));
    cursor = textdb_next_field(cursor, entry.pw_shell, sizeof(entry.pw_shell));

    entry.pw_uid = (uid_t)strtol(uid_buf, NULL, 10);
    entry.pw_gid = (gid_t)strtol(gid_buf, NULL, 10);

    if (by_name) {
        if (match_name && match_name[0] && strcmp(entry.pw_name, match_name) != 0) {
            return false;
        }
    } else if (entry.pw_uid != match_uid) {
        return false;
    }

    if (out) {
        *out = entry;
    }

    return true;
}

typedef struct {
    const char *name;
    uid_t uid;
    bool by_name;
    passwd_t *out;
} passwd_find_ctx_t;

static bool _match_passwd_line(const char *line, void *ctx_ptr) {
    if (!ctx_ptr) {
        return false;
    }

    passwd_find_ctx_t *ctx = (passwd_find_ctx_t *)ctx_ptr;
    return parse_passwd_line(line, ctx->name, ctx->uid, ctx->by_name, ctx->out);
}

static int find_passwd(const char *name, uid_t uid, bool by_name, passwd_t *out) {
    char buf[4096];
    ssize_t len = kv_read_file(PASSWD_PATH, buf, sizeof(buf));

    if (len <= 0) {
        return -1;
    }

    passwd_find_ctx_t ctx = {
        .name = name,
        .uid = uid,
        .by_name = by_name,
        .out = out,
    };
    char line[256] = {0};

    return textdb_find_line(buf, line, sizeof(line), _match_passwd_line, &ctx);
}

int getpwnam(const char *name, passwd_t *out) {
    if (!name || !out) {
        return -1;
    }

    return find_passwd(name, 0, true, out);
}

int getpwuid(uid_t uid, passwd_t *out) {
    if (!out) {
        return -1;
    }

    return find_passwd(NULL, uid, false, out);
}
