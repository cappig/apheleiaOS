#include "io.h"

#include <string.h>
#include <unistd.h>

ssize_t io_write_str(const char* text) {
    if (!text)
        return 0;

    return write(STDOUT_FILENO, text, strlen(text));
}

ssize_t io_write_char(char ch) {
    return write(STDOUT_FILENO, &ch, 1);
}
