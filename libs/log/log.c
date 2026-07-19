#include "log.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define LOG_TIME_WIDTH 5

static const char *lvl_strings[5] = {
    "debug", "info", "warn", "error", "fatal",
};

static const char *lvl_colors[5] = {
    "36;1", "32;1", "33;1", "31;1", "41;97;1",
};

typedef struct {
    int min_level;
    unsigned int options;
    char format[LOG_FORMAT_SIZE];
    puts_fn sink;
    log_clock_fn clock;
    uint64_t clock_rate;
    uint64_t clock_start;
} log_state_t;

static log_state_t log_state = {
    .min_level = LOG_INFO,
    .options = LOG_OPT_DEFAULT,
    .format = LOG_DEFAULT_FORMAT,
};

static bool _valid_level(int lvl) {
    if (lvl < LOG_DEBUG) {
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

static void _log_put_uint(char *buf, size_t *pos, uint64_t value, size_t width, char pad) {
    char digits[20];
    size_t len = 0;

    do {
        digits[len++] = (char)('0' + (value % 10));
        value /= 10;
    } while (value != 0 && len < sizeof(digits));

    while (len < width) {
        _log_putc(buf, pos, pad);
        width--;
    }

    while (len) {
        _log_putc(buf, pos, digits[--len]);
    }
}

static void _log_color(char *buf, size_t *pos, const char *color) {
    if (log_state.options & LOG_OPT_COLOR) {
        _log_puts(buf, pos, "\x1b[");
        _log_puts(buf, pos, color);
        _log_putc(buf, pos, 'm');
    }
}

static void _log_reset_color(char *buf, size_t *pos) {
    if (log_state.options & LOG_OPT_COLOR) {
        _log_puts(buf, pos, "\x1b[0m");
    }
}

static void _log_level(char *buf, size_t *pos, int lvl) {
    _log_color(buf, pos, lvl_colors[lvl]);
    _log_puts(buf, pos, lvl_strings[lvl]);
    _log_reset_color(buf, pos);
}

static void _log_location(char *buf, size_t *pos, const char *text) {
    if (!(log_state.options & LOG_OPT_LOCATION)) {
        return;
    }

    _log_puts(buf, pos, text);
}

static void _log_line(char *buf, size_t *pos, int line) {
    if (!(log_state.options & LOG_OPT_LOCATION)) {
        return;
    }

    _log_put_uint(buf, pos, (unsigned int)line, 0, '0');
}

static void _log_time(char *buf, size_t *pos) {
    uint64_t elapsed = 0;

    if (!log_state.clock || !log_state.clock_rate) {
        _log_put_uint(buf, pos, 0, LOG_TIME_WIDTH, ' ');
        _log_puts(buf, pos, ".000000");
        return;
    }

    uint64_t now = log_state.clock();
    if (now >= log_state.clock_start) {
        elapsed = now - log_state.clock_start;
    }

    uint64_t seconds = elapsed / log_state.clock_rate;
    uint64_t remainder = elapsed % log_state.clock_rate;
    uint64_t micros = (remainder * 1000000ULL) / log_state.clock_rate;

    _log_put_uint(buf, pos, seconds, LOG_TIME_WIDTH, ' ');
    _log_putc(buf, pos, '.');
    _log_put_uint(buf, pos, micros, 6, '0');
}

static void _log_layout(char *buf, int lvl, const char *file, int line, const char *message) {
    size_t pos = 0;
    const char *format = log_state.format;
    bool location_color = false;

    buf[0] = '\0';

    while (*format) {
        if (*format != '%') {
            _log_putc(buf, &pos, *format++);
            continue;
        }

        format++;

        bool location = (*format == 's' || *format == 'n') && (log_state.options & LOG_OPT_LOCATION);
        if (location && !location_color) {
            _log_color(buf, &pos, "90");
            location_color = true;
        } else if (!location && location_color) {
            _log_reset_color(buf, &pos);
            location_color = false;
        }

        switch (*format) {
        case '%':
            _log_putc(buf, &pos, '%');
            break;
        case 'l':
            _log_level(buf, &pos, lvl);
            break;
        case 'm':
            _log_puts(buf, &pos, message);
            break;
        case 'n':
            _log_line(buf, &pos, line);
            break;
        case 's':
            _log_location(buf, &pos, file);
            break;
        case 't':
            _log_time(buf, &pos);
            break;
        case '\0':
            _log_putc(buf, &pos, '%');
            continue;
        default:
            _log_putc(buf, &pos, '%');
            _log_putc(buf, &pos, *format);
            break;
        }

        format++;
    }

    if (location_color) {
        _log_reset_color(buf, &pos);
    }

    if (pos >= LOG_BUF_SIZE - 1) {
        pos = LOG_BUF_SIZE - 2;
    }

    buf[pos++] = '\n';
    buf[pos] = '\0';
}

void vslog(char *restrict buf, int lvl, const char *file, int line, const char *fmt, va_list args) {
    if (!buf) {
        return;
    }

    buf[0] = '\0';
    if (!_valid_level(lvl) || !fmt) {
        return;
    }

    char message[LOG_BUF_SIZE] = { 0 };
    vsnprintf(message, sizeof(message), fmt, args);

    _log_layout(buf, lvl, file, line, message);
}

void slog(char *restrict buf, int lvl, const char *file, int line, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);

    vslog(buf, lvl, file, line, fmt, args);

    va_end(args);
}

void log(int lvl, const char *file, int line, const char *fmt, ...) {
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
    if (lvl >= LOG_DEBUG && lvl <= LOG_NONE) {
        log_state.min_level = lvl;
    }
}

void log_set_options(unsigned int options) {
    log_state.options = options;
}

bool log_set_format(const char *format) {
    if (!format || !*format || strlen(format) >= sizeof(log_state.format)) {
        return false;
    }

    strncpy(log_state.format, format, sizeof(log_state.format) - 1);
    log_state.format[sizeof(log_state.format) - 1] = '\0';
    return true;
}

void log_set_clock(log_clock_fn clock, uint64_t rate) {
    log_state.clock = clock;
    log_state.clock_rate = rate;
    log_state.clock_start = clock && rate ? clock() : 0;
}
