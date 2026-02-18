#include <libc_usr/shadow.h>
#include <parse/textdb.h>
#include <string.h>
#include <user/kv.h>

#define SHADOW_PATH "/etc/shadow"

typedef struct {
    const char *name;
    shadow_t *out;
} shadow_find_ctx_t;

static bool _match_shadow_name(const char *line, void *ctx_ptr) {
    if (!ctx_ptr) {
        return false;
    }

    shadow_find_ctx_t *ctx = (shadow_find_ctx_t *)ctx_ptr;
    if (!ctx->name || !ctx->out) {
        return false;
    }

    shadow_t entry = {0};
    const char *field = line;

    field = textdb_next_field(field, entry.sp_name, sizeof(entry.sp_name));
    field = textdb_next_field(field, entry.sp_pwd, sizeof(entry.sp_pwd));

    if (strcmp(entry.sp_name, ctx->name) != 0) {
        return false;
    }

    *ctx->out = entry;
    return true;
}

int getspnam(const char *name, shadow_t *out) {
    if (!name || !out) {
        return -1;
    }

    char buf[4096];
    ssize_t len = kv_read_file(SHADOW_PATH, buf, sizeof(buf));

    if (len <= 0) {
        return -1;
    }

    shadow_find_ctx_t ctx = {
        .name = name,
        .out = out,
    };

    char line[256] = {0};
    return textdb_find_line(buf, line, sizeof(line), _match_shadow_name, &ctx);
}
