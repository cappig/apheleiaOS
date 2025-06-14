#pragma once

#include "stdarg.h"
#include "stddef.h"

#define MODE_READ   (1 << 0)
#define MODE_WRITE  (1 << 1)
#define MODE_APPEND (1 << 2)
#define MODE_PLUS   (1 << 3)

#define FLAG_EOF      (1 << 0)
#define FLAG_ERROR    (1 << 1)
#define FLAG_USER_BUF (1 << 2)


int vsnprintf(char* restrict buffer, size_t max_size, const char* restrict format, va_list vlist);
int vsprintf(char* restrict buffer, const char* restrict format, va_list vlist);

int snprintf(char* restrict buffer, size_t max_size, const char* restrict format, ...)
    __attribute__((format(printf, 3, 4)));
int sprintf(char* restrict buffer, const char* restrict format, ...)
    __attribute__((format(printf, 2, 3)));


int vsnscanf(const char* restrict str, size_t max, const char* restrict format, va_list vlist);
int vsscanf(const char* restrict str, const char* restrict format, va_list vlist);

int snscanf(const char* restrict str, size_t max, const char* restrict format, ...)
    __attribute__((format(scanf, 3, 4)));
int sscanf(const char* restrict str, const char* restrict format, ...)
    __attribute__((format(scanf, 2, 3)));

#ifndef _KERNEL
#include <libc_usr/stdio.h>
#endif
