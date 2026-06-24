#include <errno.h>
#include <fcntl.h>
#include <libc_usr/pwd.h>
#include <parse/textdb.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <user/kv.h>

#define PASSWD_PATH "/etc/passwd"

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

static int parse_passwd_line(const char *line, struct passwd *pwd, char *buf, size_t buflen) {
    if (!line || !pwd || !buf || !buflen) {
        return EINVAL;
    }

    char pw_name[64] = { 0 };
    char pw_passwd[128] = { 0 };
    char uid_buf[32] = { 0 };
    char gid_buf[32] = { 0 };
    char pw_gecos[128] = { 0 };
    char pw_dir[128] = { 0 };
    char pw_shell[128] = { 0 };

    const char *cursor = line;

    cursor = textdb_next_field(cursor, pw_name, sizeof(pw_name));
    cursor = textdb_next_field(cursor, pw_passwd, sizeof(pw_passwd));
    cursor = textdb_next_field(cursor, uid_buf, sizeof(uid_buf));
    cursor = textdb_next_field(cursor, gid_buf, sizeof(gid_buf));
    cursor = textdb_next_field(cursor, pw_gecos, sizeof(pw_gecos));
    cursor = textdb_next_field(cursor, pw_dir, sizeof(pw_dir));
    cursor = textdb_next_field(cursor, pw_shell, sizeof(pw_shell));

    (void)cursor;

    char *end = NULL;
    long uid = strtol(uid_buf, &end, 10);

    if (end == uid_buf || *end != '\0' || uid < 0) {
        return EINVAL;
    }

    end = NULL;
    long gid = strtol(gid_buf, &end, 10);

    if (end == gid_buf || *end != '\0' || gid < 0) {
        return EINVAL;
    }

    memset(pwd, 0, sizeof(*pwd));
    pwd->pw_uid = (uid_t)uid;
    pwd->pw_gid = (gid_t)gid;

    char *dst = buf;
    size_t left = buflen;

    int status = copy_field(&dst, &left, &pwd->pw_name, pw_name);

    if (!status) {
        status = copy_field(&dst, &left, &pwd->pw_passwd, pw_passwd);
    }
    if (!status) {
        status = copy_field(&dst, &left, &pwd->pw_gecos, pw_gecos);
    }
    if (!status) {
        status = copy_field(&dst, &left, &pwd->pw_dir, pw_dir);
    }
    if (!status) {
        status = copy_field(&dst, &left, &pwd->pw_shell, pw_shell);
    }

    return status;
}

typedef struct {
    const char *name;
    uid_t uid;
    bool by_name;
    struct passwd *pwd;
    char *buf;
    size_t buflen;
    struct passwd **result;
} passwd_lookup_t;

static int find_passwd(const passwd_lookup_t *lookup) {
    if (!lookup || !lookup->pwd || !lookup->buf || !lookup->buflen || !lookup->result) {
        return EINVAL;
    }

    char file_buf[4096];
    int fd = open(PASSWD_PATH, O_RDONLY, 0);
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

            struct passwd parsed = { 0 };
            int status = parse_passwd_line(line_copy, &parsed, lookup->buf, lookup->buflen);
            if (status && status != EINVAL) {
                return status;
            }

            if (!status) {
                if (lookup->by_name) {
                    if (lookup->name && !strcmp(parsed.pw_name, lookup->name)) {
                        *lookup->pwd = parsed;
                        *lookup->result = lookup->pwd;
                        return 0;
                    }
                } else if (parsed.pw_uid == lookup->uid) {
                    *lookup->pwd = parsed;
                    *lookup->result = lookup->pwd;
                    return 0;
                }
            }
        }

        if (!next) {
            break;
        }
        line = next + 1;
    }

    *lookup->result = NULL;
    return 0;
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
        .result = result,
    };

    return find_passwd(&lookup);
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
        .result = result,
    };

    return find_passwd(&lookup);
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
