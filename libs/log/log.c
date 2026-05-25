#include "log.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define LOG_LEVEL_WIDTH 5

static const char *lvl_strings[6] = {
    "none", "debug", "info", "warn", "error", "fatal",
};

static const char *lvl_colors[6] = {
    "37", "36", "32", "33", "31", "41;97",
};

typedef struct {
    int min_level;
    unsigned int options;
    puts_fn sink;
} log_state_t;

static log_state_t log_state = {
    .min_level = LOG_DEBUG,
    .options = LOG_OPT_DEFAULT,
};

static bool _valid_level(int lvl) {
    if (lvl < LOG_NONE) {
        return false;
    }

    return lvl <= LOG_FATAL;
}

static void _log_putc(char *buf, size_t *pos, char c) {
    if (*pos >= LOG_BUF_SIZE - 1) {
        return;
    }

    buf[(*pos)++] = c;
    buf[*pos] = '\0';
}

static void _log_puts(char *buf, size_t *pos, const char *s) {
    if (!s) {
        s = "(null)";
    }

    while (*s) {
        _log_putc(buf, pos, *s++);
    }
}

static void _log_put_dec(char *buf, size_t *pos, unsigned int value) {
    char tmp[10];
    size_t len = 0;

    do {
        tmp[len++] = (char)('0' + (value % 10U));
        value /= 10U;
    } while (value != 0 && len < sizeof(tmp));

    while (len) {
        _log_putc(buf, pos, tmp[--len]);
    }
}

static size_t _log_level_prefix(char *buf, int lvl) {
    size_t pos = 0;
    const char *name = lvl_strings[lvl];

    if (log_state.options & LOG_OPT_COLOR) {
        _log_puts(buf, &pos, "\x1b[");
        _log_puts(buf, &pos, lvl_colors[lvl]);
        _log_puts(buf, &pos, ";1m");
    }

    _log_puts(buf, &pos, name);

    size_t name_len = strlen(name);
    while (name_len < LOG_LEVEL_WIDTH) {
        _log_putc(buf, &pos, ' ');
        name_len++;
    }

    if (log_state.options & LOG_OPT_COLOR) {
        _log_puts(buf, &pos, "\x1b[0m");
    }

    _log_putc(buf, &pos, ' ');
    return pos;
}

void vslog(char *restrict buf, int lvl, char *file, int line, char *fmt, va_list args) {
    if (!buf) {
        return;
    }

    if (!_valid_level(lvl)) {
        return;
    }

    size_t prefix = _log_level_prefix(buf, lvl);

    if (log_state.options & LOG_OPT_LOCATION) {
        if (log_state.options & LOG_OPT_COLOR) {
            _log_puts(buf, &prefix, "\x1b[90m");
        }

        _log_puts(buf, &prefix, file);
        _log_putc(buf, &prefix, ':');
        _log_put_dec(buf, &prefix, (unsigned int)line);

        if (log_state.options & LOG_OPT_COLOR) {
            _log_puts(buf, &prefix, "\x1b[0m");
        }

        _log_putc(buf, &prefix, ' ');
    }

    size_t avail = LOG_BUF_SIZE - prefix;
    if (!avail) {
        return;
    }

    int printed = vsnprintf(&buf[prefix], avail, fmt, args);
    size_t end = prefix;

    if (printed >= 0) {
        end += (size_t)printed;
    }

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
    if (!_valid_level(lvl)) {
        return;
    }

    if (lvl < log_state.min_level || !log_state.sink) {
        return;
    }

    va_list args;
    va_start(args, fmt);

    char buf[LOG_BUF_SIZE];

    vslog(buf, lvl, file, line, fmt, args);

    log_state.sink(buf);

    va_end(args);
}

void log_init(puts_fn sink) {
    log_state.sink = sink;
}

void log_set_lvl(int lvl) {
    log_state.min_level = lvl;
}

void log_set_options(unsigned int options) {
    log_state.options = options;
}
