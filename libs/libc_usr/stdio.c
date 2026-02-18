#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define PRINTF_STACK_BUF 1024

int printf(char* restrict format, ...) {
    if (!format)
        return -1;

    char stack_buf[PRINTF_STACK_BUF];

    va_list arguments;
    va_start(arguments, format);
    int ret = vsnprintf(stack_buf, sizeof(stack_buf), format, arguments);
    va_end(arguments);

    if (ret < 0)
        return ret;

    size_t bytes = (size_t)ret;
    char* buf = stack_buf;

    if (bytes >= sizeof(stack_buf)) {
        buf = malloc(bytes + 1);
        if (!buf) {
            bytes = sizeof(stack_buf) - 1;
            buf = stack_buf;
        } else {
            va_start(arguments, format);
            vsnprintf(buf, bytes + 1, format, arguments);
            va_end(arguments);
        }
    }

    size_t written = 0;
    while (written < bytes) {
        ssize_t n = write(STDOUT_FILENO, buf + written, bytes - written);
        if (n <= 0) {
            if (buf != stack_buf)
                free(buf);
            return -1;
        }

        written += (size_t)n;
    }

    if (buf != stack_buf)
        free(buf);

    return ret;
}
