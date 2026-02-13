#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static bool parse_u64(const char* text, const char* key, unsigned long long* out) {
    if (!text || !key || !out)
        return false;

    size_t key_len = strlen(key);
    const char* line = text;

    while (*line) {
        const char* next = strchr(line, '\n');
        size_t line_len = next ? (size_t)(next - line) : strlen(line);

        if (line_len > key_len && !strncmp(line, key, key_len)) {
            line += key_len;
            return sscanf(line, "%llu", out) == 1;
        }

        if (!next)
            break;

        line = next + 1;
    }

    return false;
}

time_t time(time_t* timer) {
    int fd = open("/dev/clock", O_RDONLY, 0);

    if (fd < 0) {
        errno = ENOSYS;
        return (time_t)-1;
    }

    char buf[256] = {0};
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);

    if (n <= 0) {
        errno = EIO;
        return (time_t)-1;
    }

    buf[n] = '\0';

    unsigned long long now = 0;
    if (!parse_u64(buf, "now=", &now)) {
        errno = EIO;
        return (time_t)-1;
    }

    time_t value = (time_t)now;

    if (timer)
        *timer = value;

    return value;
}
