#include <fcntl.h>
#include <libc_usr/grp.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define GROUP_PATH "/etc/group"

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

static bool parse_group_line(const char* line, gid_t match_gid, group_t* out) {
    group_t entry = {0};
    char passwd_buf[32] = {0};
    char gid_buf[32] = {0};

    const char* cursor = line;

    cursor = next_field(cursor, entry.gr_name, sizeof(entry.gr_name));
    cursor = next_field(cursor, passwd_buf, sizeof(passwd_buf));
    cursor = next_field(cursor, gid_buf, sizeof(gid_buf));

    entry.gr_gid = (gid_t)strtol(gid_buf, NULL, 10);

    if (entry.gr_gid != match_gid)
        return false;

    if (out)
        *out = entry;

    return true;
}

int getgrgid(gid_t gid, group_t* out) {
    if (!out)
        return -1;

    char buf[4096];
    ssize_t len = read_file(GROUP_PATH, buf, sizeof(buf));

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

        if (parse_group_line(tmp, gid, out))
            return 0;
    }

    return -1;
}
