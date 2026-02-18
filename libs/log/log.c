#include "log.h"

#include <stdarg.h>
#include <stdio.h>

static const char *lvl_strings[6] = {"NONE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL"};
static const char *lvl_colors[6] = {"37", "36", "32", "33", "31", "41;97"};

static int min_log_lvl = LOG_DEBUG;

static puts_fn puts_ptr = NULL;


void vslog(char *restrict buf, int lvl, char *file, int line, char *fmt, va_list args) {
    if (buf == NULL) {
        return;
    }

    int prefix = snprintf(
        buf,
        LOG_BUF_SIZE,
        "\x1b[%s;1m%-5s\x1b[0;2;37m %s:%d:\x1b[0m ",
        lvl_colors[lvl],
        lvl_strings[lvl],
        file,
        line
    );

    if (prefix < 0) {
        return;
    }

    if (prefix >= LOG_BUF_SIZE) {
        buf[LOG_BUF_SIZE - 1] = '\0';
        return;
    }

    size_t avail = LOG_BUF_SIZE - (size_t)prefix;
    if (!avail) {
        return;
    }

    int printed = vsnprintf(&buf[prefix], avail, fmt, args);
    size_t end = (printed < 0) ? (size_t)prefix : (size_t)prefix + (size_t)printed;

    if (end >= LOG_BUF_SIZE - 1) {
        end = LOG_BUF_SIZE - 2;
    }

    buf[end++] = '\n';
    buf[end] = '\0';
}

void slog(char *restrict buf, int lvl, char *file, int line, char *fmt, ...) {
    va_list args;
    va_start(args, fmt);

    vslog(buf, lvl, file, line, fmt, args);

    va_end(args);
}

void log(int lvl, char *file, int line, char *fmt, ...) {
    if ((int)lvl < min_log_lvl || puts_ptr == NULL) {
        return;
    }

    va_list args;
    va_start(args, fmt);

    char buf[LOG_BUF_SIZE] = {0};

    vslog(buf, lvl, file, line, fmt, args);

    puts_ptr(buf);

    va_end(args);
}

void log_init(puts_fn puts) {
    puts_ptr = puts;
}

void log_set_lvl(int lvl) {
    min_log_lvl = lvl;
}
