#include <fcntl.h>
#include <libc_usr/shadow.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SHADOW_PATH "/etc/shadow"

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

int getspnam(const char* name, shadow_t* out) {
    if (!name || !out)
        return -1;

    char buf[4096];
    ssize_t len = read_file(SHADOW_PATH, buf, sizeof(buf));

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

        shadow_t entry = {0};
        const char* field = tmp;

        field = next_field(field, entry.sp_name, sizeof(entry.sp_name));
        field = next_field(field, entry.sp_pwd, sizeof(entry.sp_pwd));

        if (strcmp(entry.sp_name, name) != 0)
            continue;

        *out = entry;
        return 0;
    }

    return -1;
}
