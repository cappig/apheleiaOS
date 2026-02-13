#include <fcntl.h>
#include <libc_usr/pwd.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PASSWD_PATH "/etc/passwd"

static ssize_t read_file(const char* path, char* buf, size_t size) {
    if (!path || !buf || !size)
        return -1;

    int fd = open(path, O_RDONLY, 0);

    if (fd < 0)
        return -1;

    size_t total = 0;
    while (total + 1 < size) {
        ssize_t count = read(fd, buf + total, size - total - 1);

        if (count <= 0)
            break;

        total += (size_t)count;
    }

    close(fd);
    buf[total] = '\0';

    return (ssize_t)total;
}

static const char* next_field(const char* cursor, char* out, size_t out_len) {
    size_t len = 0;

    if (!out_len)
        return cursor;

    while (*cursor && *cursor != ':' && *cursor != '\n') {
        if (len + 1 < out_len)
            out[len++] = *cursor;

        cursor++;
    }

    out[len] = '\0';

    if (*cursor == ':')
        cursor++;

    return cursor;
}

static bool parse_passwd_line(
    const char* line,
    const char* match_name,
    uid_t match_uid,
    bool by_name,
    passwd_t* out
) {
    passwd_t entry = {0};
    char uid_buf[32] = {0};
    char gid_buf[32] = {0};

    const char* cursor = line;

    cursor = next_field(cursor, entry.pw_name, sizeof(entry.pw_name));
    cursor = next_field(cursor, entry.pw_passwd, sizeof(entry.pw_passwd));
    cursor = next_field(cursor, uid_buf, sizeof(uid_buf));
    cursor = next_field(cursor, gid_buf, sizeof(gid_buf));
    cursor = next_field(cursor, entry.pw_gecos, sizeof(entry.pw_gecos));
    cursor = next_field(cursor, entry.pw_dir, sizeof(entry.pw_dir));
    cursor = next_field(cursor, entry.pw_shell, sizeof(entry.pw_shell));

    entry.pw_uid = (uid_t)strtol(uid_buf, NULL, 10);
    entry.pw_gid = (gid_t)strtol(gid_buf, NULL, 10);

    if (by_name) {
        if (match_name && match_name[0] && strcmp(entry.pw_name, match_name) != 0)
            return false;
    } else if (entry.pw_uid != match_uid) {
        return false;
    }

    if (out)
        *out = entry;

    return true;
}

static int find_passwd(const char* name, uid_t uid, bool by_name, passwd_t* out) {
    char buf[4096];
    ssize_t len = read_file(PASSWD_PATH, buf, sizeof(buf));

    if (len <= 0)
        return -1;

    const char* cursor = buf;

    while (*cursor) {
        const char* line = cursor;

        while (*cursor && *cursor != '\n')
            cursor++;

        size_t line_len = (size_t)(cursor - line);

        if (*cursor == '\n')
            cursor++;

        if (!line_len)
            continue;

        if (line[0] == '#')
            continue;

        char tmp[256];

        if (line_len >= sizeof(tmp))
            line_len = sizeof(tmp) - 1;

        memcpy(tmp, line, line_len);
        tmp[line_len] = '\0';

        if (parse_passwd_line(tmp, name, uid, by_name, out))
            return 0;
    }

    return -1;
}

int getpwnam(const char* name, passwd_t* out) {
    if (!name || !out)
        return -1;

    return find_passwd(name, 0, true, out);
}

int getpwuid(uid_t uid, passwd_t* out) {
    if (!out)
        return -1;

    return find_passwd(NULL, uid, false, out);
}
