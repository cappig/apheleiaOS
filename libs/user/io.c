#include "io.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

bool io_color_enabled(int fd) {
    const char *no_color = getenv("NO_COLOR");
    return isatty(fd) && (!no_color || !no_color[0]);
}

ssize_t io_write_str(const char *text) {
    if (!text) {
        return 0;
    }

    return write(STDOUT_FILENO, text, strlen(text));
}

ssize_t io_write_char(char ch) {
    return write(STDOUT_FILENO, &ch, 1);
}

ssize_t io_write_repeat(char ch, size_t count) {
    char buf[64];
    ssize_t total = 0;

    memset(buf, ch, sizeof(buf));

    while (count > 0) {
        size_t chunk = count < sizeof(buf) ? count : sizeof(buf);
        ssize_t written = write(STDOUT_FILENO, buf, chunk);

        if (written <= 0) {
            return total ? total : written;
        }

        total += written;
        count -= (size_t)written;
    }

    return total;
}
