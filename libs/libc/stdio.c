#include "stdio.h"

#include "stdarg.h"
#include "stddef.h"


int vsprintf(char* restrict buffer, const char* restrict format, va_list vlist) {
    return vsnprintf(buffer, (size_t)-1, format, vlist);
}

int snprintf(char* restrict buffer, size_t max_size, const char* restrict format, ...) {
    va_list arguments;
    va_start(arguments, format);

    int ret = vsnprintf(buffer, max_size, format, arguments);

    va_end(arguments);
    return ret;
}

int sprintf(char* restrict buffer, const char* restrict format, ...) {
    va_list arguments;
    va_start(arguments, format);

    int ret = vsnprintf(buffer, (size_t)-1, format, arguments);

    va_end(arguments);
    return ret;
}


int vsscanf(const char* restrict str, const char* restrict format, va_list vlist) {
    return vsnscanf(str, (size_t)-1, format, vlist);
}

int snscanf(const char* restrict str, size_t max, const char* restrict format, ...) {
    va_list arguments;
    va_start(arguments, format);

    int ret = vsnscanf(str, max, format, arguments);

    va_end(arguments);
    return ret;
}

int sscanf(const char* restrict str, const char* restrict format, ...) {
    va_list arguments;
    va_start(arguments, format);

    int ret = vsnscanf(str, (size_t)-1, format, arguments);

    va_end(arguments);
    return ret;
}
