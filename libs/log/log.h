#pragma once

#include <stdarg.h>

enum log_lvl {
    LOG_DEBUG,
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR,
    LOG_FATAL,
};

typedef void (*puts_fn_ptr)(const char*);

#define LOG_BUF_SIZE 256

#define log_debug(...) log(LOG_DEBUG, __FILE__, __LINE__, __VA_ARGS__)
#define log_info(...)  log(LOG_INFO, __FILE__, __LINE__, __VA_ARGS__)
#define log_warn(...)  log(LOG_WARN, __FILE__, __LINE__, __VA_ARGS__)
#define log_error(...) log(LOG_ERROR, __FILE__, __LINE__, __VA_ARGS__)
#define log_fatal(...) log(LOG_FATAL, __FILE__, __LINE__, __VA_ARGS__)


void vslog(char* restrict buf, int lvl, char* file, int line, char* fmt, va_list args);

void slog(char* restrict buf, int lvl, char* file, int line, char* fmt, ...)
    __attribute__((format(printf, 5, 6)));
void log(enum log_lvl lvl, char* file, int line, char* fmt, ...) __attribute__((format(printf, 4, 5)));

void log_init(puts_fn_ptr puts);
void log_set_lvl(int lvl);
