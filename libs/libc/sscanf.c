#include "ctype.h"
#include "limits.h"
#include "stdarg.h"
#include "stdbool.h"
#include "stddef.h"
#include "stdint.h"
#include "stdio.h"
#include "sys/types.h"

typedef enum {
    SCAN_CHAR,
    SCAN_SHORT,
    SCAN_INT,
    SCAN_LONG,
    SCAN_LLONG,
    SCAN_SIZE,
    SCAN_PTRDIFF,
    SCAN_INTMAX,
    SCAN_PTR,
} scan_size_t;

typedef struct {
    const char *text;
    size_t pos;
    size_t max;
} scan_input_t;

static bool scan_end(const scan_input_t *in) {
    return !in || in->pos >= in->max || in->text[in->pos] == '\0';
}

static void scan_space(scan_input_t *in) {
    while (!scan_end(in) && isspace((unsigned char)in->text[in->pos])) {
        in->pos++;
    }
}

static size_t read_width(const char *format, size_t *index) {
    if (!isdigit((unsigned char)format[*index])) {
        return SIZE_MAX;
    }

    size_t width = 0;
    while (isdigit((unsigned char)format[*index])) {
        unsigned digit = (unsigned)(format[*index] - '0');
        if (width > (SIZE_MAX - digit) / 10) {
            width = SIZE_MAX;
        } else {
            width = width * 10 + digit;
        }
        (*index)++;
    }

    return width;
}

static scan_size_t read_size(const char *format, size_t *index) {
    switch (format[*index]) {
    case 'h':
        (*index)++;
        if (format[*index] == 'h') {
            (*index)++;
            return SCAN_CHAR;
        }
        return SCAN_SHORT;
    case 'l':
        (*index)++;
        if (format[*index] == 'l') {
            (*index)++;
            return SCAN_LLONG;
        }
        return SCAN_LONG;
    case 'z':
        (*index)++;
        return SCAN_SIZE;
    case 't':
        (*index)++;
        return SCAN_PTRDIFF;
    case 'j':
        (*index)++;
        return SCAN_INTMAX;
    default:
        return SCAN_INT;
    }
}

static void store_signed(void *ptr, scan_size_t size, long long value) {
    switch (size) {
    case SCAN_CHAR:
        *(signed char *)ptr = (signed char)value;
        break;
    case SCAN_SHORT:
        *(short *)ptr = (short)value;
        break;
    case SCAN_LONG:
        *(long *)ptr = (long)value;
        break;
    case SCAN_LLONG:
        *(long long *)ptr = value;
        break;
    case SCAN_SIZE:
        *(ssize_t *)ptr = (ssize_t)value;
        break;
    case SCAN_PTRDIFF:
        *(ptrdiff_t *)ptr = (ptrdiff_t)value;
        break;
    case SCAN_INTMAX:
        *(intmax_t *)ptr = (intmax_t)value;
        break;
    default:
        *(int *)ptr = (int)value;
        break;
    }
}

static void store_unsigned(void *ptr, scan_size_t size, unsigned long long value) {
    switch (size) {
    case SCAN_CHAR:
        *(unsigned char *)ptr = (unsigned char)value;
        break;
    case SCAN_SHORT:
        *(unsigned short *)ptr = (unsigned short)value;
        break;
    case SCAN_LONG:
        *(unsigned long *)ptr = (unsigned long)value;
        break;
    case SCAN_LLONG:
        *(unsigned long long *)ptr = value;
        break;
    case SCAN_SIZE:
        *(size_t *)ptr = (size_t)value;
        break;
    case SCAN_PTRDIFF:
        *(uintptr_t *)ptr = (uintptr_t)value;
        break;
    case SCAN_INTMAX:
        *(uintmax_t *)ptr = (uintmax_t)value;
        break;
    case SCAN_PTR:
        *(void **)ptr = (void *)(uintptr_t)value;
        break;
    default:
        *(unsigned int *)ptr = (unsigned int)value;
        break;
    }
}

static int digit_value(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'z') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'Z') {
        return ch - 'A' + 10;
    }
    return -1;
}

static bool scan_number(scan_input_t *in, size_t width, char spec, unsigned long long *value_out, bool *negative_out) {
    size_t start = in->pos;
    size_t used = 0;
    bool negative = false;

    if (width && !scan_end(in) && (in->text[in->pos] == '+' || in->text[in->pos] == '-')) {
        negative = in->text[in->pos] == '-';
        in->pos++;
        used++;
    }

    int base = 10;
    if (spec == 'o') {
        base = 8;
    } else if (spec == 'x' || spec == 'X' || spec == 'p') {
        base = 16;
    } else if (spec == 'b') {
        base = 2;
    } else if (spec == 'i') {
        base = 0;
    }

    size_t left = width == SIZE_MAX ? SIZE_MAX : width - used;
    if ((base == 0 || base == 16) && left >= 2 && !scan_end(in) && in->text[in->pos] == '0' && in->pos + 1 < in->max &&
        (in->text[in->pos + 1] == 'x' || in->text[in->pos + 1] == 'X')) {
        base = 16;
        in->pos += 2;
        used += 2;
    } else if (base == 0) {
        base = !scan_end(in) && in->text[in->pos] == '0' ? 8 : 10;
    }

    size_t digits = 0;
    unsigned long long value = 0;
    while (!scan_end(in) && used < width) {
        int digit = digit_value(in->text[in->pos]);
        if (digit < 0 || digit >= base) {
            break;
        }

        value = value * (unsigned)base + (unsigned)digit;
        in->pos++;
        used++;
        digits++;
    }

    if (!digits) {
        in->pos = start;
        return false;
    }

    *value_out = value;
    *negative_out = negative;
    return true;
}

static bool set_has(const char *begin, const char *end, unsigned char ch) {
    for (const char *p = begin; p < end; p++) {
        if (p + 2 < end && p[1] == '-') {
            unsigned char first = (unsigned char)p[0];
            unsigned char last = (unsigned char)p[2];
            if (first <= ch && ch <= last) {
                return true;
            }
            p += 2;
        } else if ((unsigned char)*p == ch) {
            return true;
        }
    }
    return false;
}

static const char *set_end(const char *format) {
    const char *p = format;
    if (*p == '^') {
        p++;
    }
    if (*p == ']') {
        p++;
    }
    while (*p && *p != ']') {
        p++;
    }
    return *p == ']' ? p : NULL;
}

static bool
scan_text(scan_input_t *in, size_t width, char spec, const char *set, const char *set_limit, bool invert, char *out) {
    if (spec == 's') {
        scan_space(in);
    }

    if (width == SIZE_MAX && spec == 'c') {
        width = 1;
    }

    size_t start = in->pos;
    size_t count = 0;
    while (!scan_end(in) && count < width) {
        unsigned char ch = (unsigned char)in->text[in->pos];
        bool accept = true;
        if (spec == 's') {
            accept = !isspace(ch);
        } else if (spec == '[') {
            accept = set_has(set, set_limit, ch) != invert;
        }

        if (!accept) {
            break;
        }

        in->pos++;
        count++;
    }

    if (!count) {
        in->pos = start;
        return false;
    }

    if (spec == 'c' && count != width) {
        return false;
    }

    if (out) {
        for (size_t i = 0; i < count; i++) {
            out[i] = in->text[start + i];
        }
        if (spec != 'c') {
            out[count] = '\0';
        }
    }
    return true;
}

static void store_count(void *ptr, scan_size_t size, size_t value) {
    store_signed(ptr, size, (long long)value);
}

int vsnscanf(const char *restrict str, size_t max, const char *restrict format, va_list args) {
    if (!str || !format) {
        return EOF;
    }

    scan_input_t in = {
        .text = str,
        .max = max,
    };
    int assigned = 0;

    for (size_t i = 0; format[i]; i++) {
        if (isspace((unsigned char)format[i])) {
            while (isspace((unsigned char)format[i + 1])) {
                i++;
            }
            scan_space(&in);
            continue;
        }

        if (format[i] != '%') {
            if (scan_end(&in)) {
                return assigned ? assigned : EOF;
            }
            if (str[in.pos] != format[i]) {
                break;
            }
            in.pos++;
            continue;
        }

        i++;
        if (format[i] == '%') {
            if (scan_end(&in)) {
                return assigned ? assigned : EOF;
            }
            if (str[in.pos] != '%') {
                break;
            }
            in.pos++;
            continue;
        }

        bool ignore = false;
        if (format[i] == '*') {
            ignore = true;
            i++;
        }

        size_t width = read_width(format, &i);
        scan_size_t size = read_size(format, &i);
        char spec = format[i];
        if (!spec) {
            break;
        }

        if (spec == 'n') {
            if (!ignore) {
                store_count(va_arg(args, void *), size, in.pos);
            }
            continue;
        }

        if (spec != 'c' && spec != '[') {
            scan_space(&in);
        }

        if (scan_end(&in)) {
            return assigned ? assigned : EOF;
        }

        bool matched = false;
        if (spec == 'd' || spec == 'i' || spec == 'u' || spec == 'o' || spec == 'x' || spec == 'X' || spec == 'p' ||
            spec == 'b') {
            unsigned long long value = 0;
            bool negative = false;
            matched = scan_number(&in, width, spec, &value, &negative);
            if (matched && !ignore) {
                if (spec == 'd' || spec == 'i') {
                    long long signed_value = negative ? (long long)(0ULL - value) : (long long)value;
                    store_signed(va_arg(args, void *), size, signed_value);
                } else {
                    if (negative) {
                        value = 0ULL - value;
                    }
                    store_unsigned(va_arg(args, void *), spec == 'p' ? SCAN_PTR : size, value);
                }
            }
        } else if (spec == 'c' || spec == 's') {
            char *out = ignore ? NULL : va_arg(args, char *);
            matched = scan_text(&in, width, spec, NULL, NULL, false, out);
        } else if (spec == '[') {
            const char *begin = &format[i + 1];
            const char *end = set_end(begin);
            if (!end) {
                break;
            }

            bool invert = *begin == '^';
            if (invert) {
                begin++;
            }
            char *out = ignore ? NULL : va_arg(args, char *);
            matched = scan_text(&in, width, spec, begin, end, invert, out);
            i = (size_t)(end - format);
        } else {
            break;
        }

        if (!matched) {
            if (scan_end(&in)) {
                return assigned ? assigned : EOF;
            }
            break;
        }
        if (!ignore) {
            assigned++;
        }
    }

    return assigned;
}
