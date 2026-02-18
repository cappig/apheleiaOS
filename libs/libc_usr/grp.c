#include <libc_usr/grp.h>
#include <parse/textdb.h>
#include <stdlib.h>
#include <string.h>
#include <user/kv.h>

#define GROUP_PATH "/etc/group"

static bool parse_group_line(const char *line, gid_t match_gid, group_t *out) {
    group_t entry = {0};
    char passwd_buf[32] = {0};
    char gid_buf[32] = {0};

    const char *cursor = line;

    cursor = textdb_next_field(cursor, entry.gr_name, sizeof(entry.gr_name));
    cursor = textdb_next_field(cursor, passwd_buf, sizeof(passwd_buf));
    cursor = textdb_next_field(cursor, gid_buf, sizeof(gid_buf));

    entry.gr_gid = (gid_t)strtol(gid_buf, NULL, 10);

    if (entry.gr_gid != match_gid) {
        return false;
    }

    if (out) {
        *out = entry;
    }

    return true;
}

typedef struct {
    gid_t gid;
    group_t *out;
} group_find_ctx_t;

static bool _match_group_line(const char *line, void *ctx_ptr) {
    if (!ctx_ptr) {
        return false;
    }

    group_find_ctx_t *ctx = (group_find_ctx_t *)ctx_ptr;
    return parse_group_line(line, ctx->gid, ctx->out);
}

int getgrgid(gid_t gid, group_t *out) {
    if (!out) {
        return -1;
    }

    char buf[4096];
    ssize_t len = kv_read_file(GROUP_PATH, buf, sizeof(buf));

    if (len <= 0) {
        return -1;
    }

    group_find_ctx_t ctx = {
        .gid = gid,
        .out = out,
    };
    char line[256] = {0};

    return textdb_find_line(buf, line, sizeof(line), _match_group_line, &ctx);
}
