#pragma once

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>

enum log_levels {
    LOG_DEBUG = 0,
    LOG_INFO = 1,
    LOG_WARN = 2,
    LOG_ERROR = 3,
    LOG_FATAL = 4,
    LOG_NONE = 5,
};

enum log_options {
    LOG_OPT_NONE = 0,
    LOG_OPT_COLOR = 1U << 0,
    LOG_OPT_LOCATION = 1U << 1,
    LOG_OPT_DEFAULT = LOG_OPT_COLOR | LOG_OPT_LOCATION,
};

typedef void (*puts_fn)(const char *);
typedef uint64_t (*log_clock_fn)(void);

#define LOG_BUF_SIZE       256
#define LOG_FORMAT_SIZE    96
#define LOG_DEFAULT_FORMAT "%m"

#define log_debug(...) log(LOG_DEBUG, __FILE__, __LINE__, __VA_ARGS__)
#define log_info(...)  log(LOG_INFO, __FILE__, __LINE__, __VA_ARGS__)
#define log_warn(...)  log(LOG_WARN, __FILE__, __LINE__, __VA_ARGS__)
#define log_error(...) log(LOG_ERROR, __FILE__, __LINE__, __VA_ARGS__)
#define log_fatal(...) log(LOG_FATAL, __FILE__, __LINE__, __VA_ARGS__)


void vslog(char *restrict buf, int lvl, const char *file, int line, const char *fmt, va_list args);

void slog(char *restrict buf, int lvl, const char *file, int line, const char *fmt, ...)
    __attribute__((format(printf, 5, 6)));
void log(int lvl, const char *file, int line, const char *fmt, ...) __attribute__((format(printf, 4, 5)));

void log_init(puts_fn sink);
void log_set_lvl(int lvl);
void log_set_options(unsigned int options);
bool log_set_format(const char *format);
void log_set_clock(log_clock_fn clock, uint64_t rate);
