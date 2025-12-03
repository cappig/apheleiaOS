#pragma once

#include <stdarg.h>

enum log_levels {
    LOG_NONE = 0,
    LOG_DEBUG = 1,
    LOG_INFO = 2,
    LOG_WARN = 3,
    LOG_ERROR = 4,
    LOG_FATAL = 5,
};

typedef void (*puts_fn)(const char*);

#define LOG_BUF_SIZE 256

#define log_debug(...) log(LOG_DEBUG, __FILE__, __LINE__, __VA_ARGS__)
#define log_info(...)  log(LOG_INFO, __FILE__, __LINE__, __VA_ARGS__)
#define log_warn(...)  log(LOG_WARN, __FILE__, __LINE__, __VA_ARGS__)
#define log_error(...) log(LOG_ERROR, __FILE__, __LINE__, __VA_ARGS__)
#define log_fatal(...) log(LOG_FATAL, __FILE__, __LINE__, __VA_ARGS__)


void vslog(char* restrict buf, int lvl, char* file, int line, char* fmt, va_list args);

void slog(char* restrict buf, int lvl, char* file, int line, char* fmt, ...)
    __attribute__((format(printf, 5, 6)));
void log(int lvl, char* file, int line, char* fmt, ...) __attribute__((format(printf, 4, 5)));

void log_init(puts_fn puts);
void log_set_lvl(int lvl);
