#include "io.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

bool io_color_enabled(int fd) {
    const char *no_color = getenv("NO_COLOR");
    return isatty(fd) && (!no_color || !no_color[0]);
}

bool io_write_all(int fd, const void *buf, size_t len) {
    const char *cursor = buf;
    size_t off = 0;

    while (off < len) {
        ssize_t written = write(fd, cursor + off, len - off);

        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            return false;
        }

        off += (size_t)written;
    }

    return true;
}

bool io_write_str(const char *text) {
    if (!text) {
        return true;
    }

    return io_write_all(STDOUT_FILENO, text, strlen(text));
}

bool io_write_char(char ch) {
    return io_write_all(STDOUT_FILENO, &ch, 1);
}

bool io_write_repeat(char ch, size_t count) {
    char buf[64];

    memset(buf, ch, sizeof(buf));

    while (count) {
        size_t chunk = count < sizeof(buf) ? count : sizeof(buf);

        if (!io_write_all(STDOUT_FILENO, buf, chunk)) {
            return false;
        }

        count -= chunk;
    }

    return true;
}

ssize_t io_read_line(int fd, char *buf, size_t len) {
    if (!buf || len < 2) {
        return -1;
    }

    size_t pos = 0;

    for (;;) {
        char ch = 0;
        ssize_t count = read(fd, &ch, 1);

        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            return -1;
        }
        if (!count) {
            break;
        }
        if (ch == '\n' || ch == '\r') {
            buf[pos] = '\0';
            return (ssize_t)pos;
        }

        // swallow the oversized remainder so the next line starts clean
        if (pos + 1 >= len) {
            while (count > 0 && ch != '\n' && ch != '\r') {
                count = read(fd, &ch, 1);
                if (count < 0 && errno == EINTR) {
                    count = 1;
                    ch = 0;
                }
            }

            buf[0] = '\0';
            return -2;
        }

        buf[pos++] = ch;
    }

    buf[pos] = '\0';
    return pos ? (ssize_t)pos : -1;
}
