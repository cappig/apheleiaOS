#include <errno.h>
#include <fcntl.h>
#include <libc_usr/shadow.h>
#include <parse/textdb.h>
#include <string.h>
#include <unistd.h>
#include <user/kv.h>

#define SHADOW_PATH "/etc/shadow"

static int
copy_field(char **cursor, size_t *left, char **out, const char *src) {
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

static int parse_shadow_line(
    const char *line,
    struct spwd *spbuf,
    char *buf,
    size_t buflen
) {
    if (!line || !spbuf || !buf || !buflen) {
        return EINVAL;
    }

    char name[64] = {0};
    char pwd[128] = {0};

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

    int rc = copy_field(&dst, &left, &spbuf->sp_namp, name);

    if (!rc) {
        rc = copy_field(&dst, &left, &spbuf->sp_pwdp, pwd);
    }

    return rc;
}

int getspnam_r(
    const char *name,
    struct spwd *spbuf,
    char *buf,
    size_t buflen,
    struct spwd **result
) {
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
            char tmp[256];
            if (line_len >= sizeof(tmp)) {
                return ERANGE;
            }

            memcpy(tmp, line, line_len);
            tmp[line_len] = '\0';

            struct spwd parsed = {0};
            int rc = parse_shadow_line(tmp, &parsed, buf, buflen);

            if (rc && rc != EINVAL) {
                return rc;
            }

            if (!rc && !strcmp(parsed.sp_namp, name)) {
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

    int rc = getspnam_r(name, &sp, buf, sizeof(buf), &result);

    if (rc || !result) {
        errno = rc ? rc : ENOENT;
        return NULL;
    }

    return result;
}
