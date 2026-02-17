#include <stdio.h>
#include <unistd.h>

int printf(char* restrict format, ...) {
    if (!format)
        return -1;

    char buffer[1024];

    va_list arguments;
    va_start(arguments, format);
    int ret = vsnprintf(buffer, sizeof(buffer), format, arguments);
    va_end(arguments);

    if (ret < 0)
        return ret;

    size_t bytes = (size_t)ret;
    if (bytes >= sizeof(buffer))
        bytes = sizeof(buffer) - 1;

    size_t written = 0;
    while (written < bytes) {
        ssize_t n = write(STDOUT_FILENO, buffer + written, bytes - written);
        if (n <= 0)
            return -1;

        written += (size_t)n;
    }

    return ret;
}
