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
