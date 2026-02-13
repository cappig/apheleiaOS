#include "kv.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

ssize_t kv_read_file(const char* path, char* out, size_t out_len) {
    if (!path || !out || out_len < 2)
        return -1;

    int fd = open(path, O_RDONLY, 0);
    if (fd < 0)
        return -1;

    size_t off = 0;

    while (off + 1 < out_len) {
        ssize_t got = read(fd, out + off, out_len - off - 1);

        if (!got)
            break;

        if (got < 0) {
            close(fd);
            return -1;
        }

        off += (size_t)got;
    }

    close(fd);
    out[off] = '\0';

    return (ssize_t)off;
}

bool kv_read_string(const char* text, const char* key, char* out, size_t out_len) {
    if (!text || !key || !out || out_len < 2)
        return false;

    size_t key_len = strlen(key);
    const char* line = text;

    while (*line) {
        const char* next = strchr(line, '\n');
        size_t line_len = next ? (size_t)(next - line) : strlen(line);

        if (line_len > key_len + 1 && !strncmp(line, key, key_len) && line[key_len] == '=') {
            size_t value_len = line_len - key_len - 1;

            if (value_len >= out_len)
                value_len = out_len - 1;

            memcpy(out, line + key_len + 1, value_len);
            out[value_len] = '\0';

            return true;
        }

        if (!next)
            break;

        line = next + 1;
    }

    return false;
}

bool kv_read_u64(const char* text, const char* key, unsigned long long* out) {
    if (!out)
        return false;

    char value[64] = {0};

    if (!kv_read_string(text, key, value, sizeof(value)))
        return false;

    return sscanf(value, "%llu", out) == 1;
}
