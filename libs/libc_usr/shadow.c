#include <errno.h>
#include <fcntl.h>
#include <libc_usr/shadow.h>
#include <parse/textdb.h>
#include <string.h>
#include <unistd.h>
#include <user/kv.h>

#define SHADOW_PATH "/etc/shadow"

static int copy_field(char **cursor, size_t *left, char **out, const char *src) {
    if (!cursor || !left || !out || !src) {
        return ERANGE;
    }

    size_t n = strlen(src) + 1;
    if (n > *left) {
        return ERANGE;
    }

    memcpy(*cursor, src, n);
    *out = *cursor;
    *cursor += n;
    *left -= n;

    return 0;
}

static int parse_shadow_line(const char *line, struct spwd *spbuf, char *buf, size_t buflen) {
    if (!line || !spbuf || !buf || !buflen) {
        return EINVAL;
    }

    char name[64] = { 0 };
    char pwd[128] = { 0 };

    const char *cursor = line;

    cursor = textdb_next_field(cursor, name, sizeof(name));
    cursor = textdb_next_field(cursor, pwd, sizeof(pwd));

    (void)cursor;

    memset(spbuf, 0, sizeof(*spbuf));
    spbuf->sp_lstchg = -1;
    spbuf->sp_min = -1;
    spbuf->sp_max = -1;
    spbuf->sp_warn = -1;
    spbuf->sp_inact = -1;
    spbuf->sp_expire = -1;
    spbuf->sp_flag = 0;

    char *dst = buf;
    size_t left = buflen;

    int status = copy_field(&dst, &left, &spbuf->sp_namp, name);

    if (!status) {
        status = copy_field(&dst, &left, &spbuf->sp_pwdp, pwd);
    }

    return status;
}

int getspnam_r(const char *name, struct spwd *spbuf, char *buf, size_t buflen, struct spwd **result) {
    if (!name || !spbuf || !buf || !buflen || !result) {
        return EINVAL;
    }

    char file_buf[4096];
    int fd = open(SHADOW_PATH, O_RDONLY, 0);
    if (fd < 0) {
        return ENOENT;
    }

    ssize_t len = kv_read_fd(fd, file_buf, sizeof(file_buf));
    close(fd);
    if (len <= 0) {
        return ENOENT;
    }

    const char *line = file_buf;
    while (*line) {
        const char *next = strchr(line, '\n');
        size_t line_len = next ? (size_t)(next - line) : strlen(line);

        if (line_len) {
            char line_copy[256];
            if (line_len >= sizeof(line_copy)) {
                return ERANGE;
            }

            memcpy(line_copy, line, line_len);
            line_copy[line_len] = '\0';

            struct spwd parsed = { 0 };
            int status = parse_shadow_line(line_copy, &parsed, buf, buflen);

            if (status && status != EINVAL) {
                return status;
            }

            if (!status && !strcmp(parsed.sp_namp, name)) {
                *spbuf = parsed;
                *result = spbuf;
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

struct spwd *getspnam(const char *name) {
    static struct spwd sp;
    static char buf[256];
    struct spwd *result = NULL;

    int status = getspnam_r(name, &sp, buf, sizeof(buf), &result);

    if (status || !result) {
        errno = status ? status : ENOENT;
        return NULL;
    }

    return result;
}
