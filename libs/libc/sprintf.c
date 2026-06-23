#include "ctype.h"
#include "limits.h"
#include "stdarg.h"
#include "stdbool.h"
#include "stddef.h"
#include "stdint.h"
#include "stdlib.h"
#include "string.h"

#define FLAGS_MINUS (1 << 0)
#define FLAGS_PLUS  (1 << 1)
#define FLAGS_SPACE (1 << 2)
#define FLAGS_HASH  (1 << 3)
#define FLAGS_ZERO  (1 << 4)

static int _get_flags(const char *format, size_t *index) {
    int flags = 0;

    bool more_flags = true;
    do {
        switch (format[(*index)++]) {
        case '-':
            flags |= FLAGS_MINUS;
            break;
        case '+':
            flags |= FLAGS_PLUS;
            break;
        case ' ':
            flags |= FLAGS_SPACE;
            break;
        case '#':
            flags |= FLAGS_HASH;
            break;
        case '0':
            flags |= FLAGS_ZERO;
            break;

        default:
            more_flags = false;
            (*index)--;
            break;
        }
    } while (more_flags);

    if ((flags & FLAGS_SPACE) && (flags & FLAGS_PLUS)) {
        flags &= ~FLAGS_SPACE;
    }

    return flags;
}

static int _get_width(const char *format, size_t *index, va_list *vlist) {
    int width = 0;

    if (isdigit((unsigned char)format[*index])) {
        while (isdigit((unsigned char)format[*index])) {
            width = width * 10 + (format[*index] - '0');
            *index += 1;
        }
    } else if (format[*index] == '*') {
        width = va_arg(*vlist, int);
        *index += 1;
    }

    return width;
}

static int _get_precision(const char *format, size_t *index, va_list *vlist) {
    if (format[*index] != '.') {
        return -1;
    }

    (*index)++;

    int precision = -1;

    if (isdigit((unsigned char)format[*index])) {
        precision = 0;

        while (isdigit((unsigned char)format[*index])) {
            precision = precision * 10 + (format[*index] - '0');
            *index += 1;
        }
    } else if (format[*index] == '*') {
        precision = va_arg(*vlist, int);
        *index += 1;
    }

    return precision;
}

// negative values represent signed types
#define SIZE_CHAR    1
#define SIZE_SHORT   2
#define SIZE_INT     3
#define SIZE_LONG    4
#define SIZE_LLONG   5
#define SIZE_SIZE    6
#define SIZE_PTRDIFF 7
#define SIZE_INTMAX  8
#define SIZE_PTR     9

static int _get_size(const char *format, size_t *index) {
    int size = SIZE_INT;

    switch (format[(*index)++]) {
    case 'h':
        if (format[*index] == 'h') {
            size = SIZE_CHAR;
            (*index)++;
        } else {
            size = SIZE_SHORT;
        }
        break;
    case 'l':
        if (format[*index] == 'l') {
            size = SIZE_LLONG;
            (*index)++;
        } else {
            size = SIZE_LONG;
        }
        break;
    case 'z':
        size = SIZE_SIZE;
        break;
    case 'j':
        size = SIZE_INTMAX;
        break;
    case 't':
        size = SIZE_PTRDIFF;
        break;
    default:
        (*index)--;
        break;
    }

    return size;
}

static uintmax_t _get_var_number(int size, va_list *vlist) {
    switch (size) {
    case SIZE_LLONG:
        return va_arg(*vlist, unsigned long long);
    case -SIZE_LLONG:
        return (uintmax_t)va_arg(*vlist, long long);

    case SIZE_LONG:
        return va_arg(*vlist, unsigned long);
    case -SIZE_LONG:
        return (uintmax_t)va_arg(*vlist, long);

    default:
    case SIZE_INT:
        return va_arg(*vlist, unsigned int);

    case SIZE_SHORT:
        return (uintmax_t)(unsigned short)va_arg(*vlist, unsigned int);
    case SIZE_CHAR:
        return (uintmax_t)(unsigned char)va_arg(*vlist, unsigned int);

    case -SIZE_INT:
        return (uintmax_t)va_arg(*vlist, int);

    case -SIZE_SHORT:
        return (uintmax_t)(intmax_t)(short)va_arg(*vlist, int);
    case -SIZE_CHAR:
        return (uintmax_t)(intmax_t)(signed char)va_arg(*vlist, int);

    case SIZE_SIZE:
    case -SIZE_SIZE:
        return (uintmax_t)va_arg(*vlist, size_t);

    case SIZE_PTRDIFF:
    case -SIZE_PTRDIFF:
        return (uintmax_t)va_arg(*vlist, ptrdiff_t);

    case SIZE_INTMAX:
        return va_arg(*vlist, uintmax_t);
    case -SIZE_INTMAX:
        return (uintmax_t)va_arg(*vlist, intmax_t);

    case SIZE_PTR:
    case -SIZE_PTR:
        return (uintptr_t)va_arg(*vlist, void *);
    }
}

typedef struct {
    char *buf;
    size_t cap;
    size_t *written;
} fmt_out_t;

typedef struct {
    int flags;
    int width;
    int precision;
    int size;
    int base;
    bool uppercase;
} fmt_spec_t;

static void _buf_putc(fmt_out_t *out, char value) {
    if (!out || !out->written) {
        return;
    }

    size_t pos = *out->written;
    if (out->buf && out->cap && pos + 1 < out->cap) {
        out->buf[pos] = value;
    }

    *out->written = pos + 1;
}

static void _string_to_buffer(fmt_out_t *out, char *string, const fmt_spec_t *spec, int *padding) {
    if (!(spec->flags & FLAGS_MINUS) && !(spec->flags & FLAGS_ZERO)) {
        while ((*padding)-- > 0) {
            _buf_putc(out, ' ');
        }
    }

    size_t len = (spec->precision < 0) ? SIZE_MAX : (size_t)spec->precision;

    for (size_t i = 0; i < len && *string; i++) {
        _buf_putc(out, *string++);
    }

    if ((spec->flags & FLAGS_MINUS) && !(spec->flags & FLAGS_ZERO)) {
        while ((*padding)-- > 0) {
            _buf_putc(out, ' ');
        }
    }
}

static size_t _bounded_strlen(const char *string, int precision) {
    if (precision < 0) {
        return strlen(string);
    }

    size_t len = 0;
    size_t max_len = (size_t)precision;

    while (len < max_len && string[len]) {
        len++;
    }

    return len;
}

static int _padding_for_width(int width, size_t len) {
    if (width <= 0 || (size_t)width <= len) {
        return 0;
    }

    return (int)((size_t)width - len);
}

static int _num_prefix_len(uintmax_t number, const fmt_spec_t *spec) {
    int len = 0;

    if (spec->size < 0 && (intmax_t)number < 0) {
        len++;
    } else if (spec->flags & (FLAGS_PLUS | FLAGS_SPACE)) {
        len++;
    }

    if (spec->flags & FLAGS_HASH) {
        if (spec->base == 2 && number != 0) {
            len += 2;
        } else if (spec->base == 16 && (number != 0 || spec->size == SIZE_PTR)) {
            len += 2;
        } else if (spec->base == 8) {
            len++;
        }
    }

    return len;
}

static uintmax_t _signed_abs(uintmax_t number) {
    intmax_t value = (intmax_t)number;
    if (value >= 0) {
        return (uintmax_t)value;
    }

    // intmax_min cannot be negated directly
    return (uintmax_t)(-(value + 1)) + 1;
}

static void _append_num_prefix(fmt_out_t *out, uintmax_t number, const fmt_spec_t *spec) {
    char sign = 0;
    if (spec->size < 0 && (intmax_t)number < 0) {
        sign = '-';
    } else if (spec->flags & FLAGS_PLUS) {
        sign = '+';
    } else if (spec->flags & FLAGS_SPACE) {
        sign = ' ';
    }

    if (sign) {
        _buf_putc(out, sign);
    }

    const char *prefix = "";
    if (spec->base == 2 && number != 0) {
        prefix = "0b";
    } else if (spec->base == 8) {
        prefix = "0";
    } else if (spec->base == 16 && (number != 0 || spec->size == SIZE_PTR)) {
        prefix = spec->uppercase ? "0X" : "0x";
    }

    while (*prefix && (spec->flags & FLAGS_HASH)) {
        _buf_putc(out, *prefix++);
    }
}

// string and char are special
#define BASE_STRING  -314
#define BASE_CHAR    -271
#define BASE_UNKNOWN 0
// these represent an actual radix, we use the minus like a flag
#define BASE_BIN  2
#define BASE_OCT  8
#define BASE_UDEC 10
#define BASE_SDEC -10
#define BASE_HEX  16
#define BASE_PTR  -16

static int _get_base(char type) {
    switch (type) {
    case 'p':
        return BASE_PTR;
    case 'x':
    case 'X':
        return BASE_HEX;

    case 'i':
    case 'd':
        return BASE_SDEC;
    case 'u':
        return BASE_UDEC;

    case 'o':
        return BASE_OCT;

    case 'b':
        return BASE_BIN;

    case 'c':
        return BASE_CHAR;

    case 's':
        return BASE_STRING;

    default:
        return BASE_UNKNOWN;
    }
}

static void _format_text(fmt_out_t *out, fmt_spec_t spec, va_list *args) {
    char char_holder[2] = { 0 };
    char *string;
    spec.flags &= ~FLAGS_ZERO;

    if (spec.base == BASE_STRING) {
        string = va_arg(*args, char *);

        if (!string) {
            string = "(null)";
        }
    } else {
        char_holder[0] = (char)va_arg(*args, int);
        string = char_holder;
    }

    if (spec.base == BASE_CHAR) {
        int padding = _padding_for_width(spec.width, 1);

        if (!(spec.flags & FLAGS_MINUS)) {
            while (padding-- > 0) {
                _buf_putc(out, ' ');
            }
        }

        _buf_putc(out, char_holder[0]);

        if (spec.flags & FLAGS_MINUS) {
            while (padding-- > 0) {
                _buf_putc(out, ' ');
            }
        }

        return;
    }

    size_t len = _bounded_strlen(string, spec.precision);
    int padding = _padding_for_width(spec.width, len);
    _string_to_buffer(out, string, &spec, &padding);
}

static void _format_number(fmt_out_t *out, fmt_spec_t spec, va_list *args) {
    uintmax_t number = _get_var_number(spec.size, args);
    bool negative = (spec.size < 0 && (intmax_t)number < 0);

    if (spec.precision >= 0) {
        spec.flags &= ~FLAGS_ZERO;
    }

    char num_buffer[66] = { 0 };
    uintmax_t absval = negative ? _signed_abs(number) : number;

    int len = (int)ulltoa((unsigned long long)absval, num_buffer, spec.base);
    if (spec.precision == 0 && absval == 0) {
        num_buffer[0] = '\0';
        len = 0;
    }

    int prefix_len = _num_prefix_len(number, &spec);
    int zeroes = 0;
    if (spec.precision > len) {
        zeroes = spec.precision - len;
    }

    size_t field_len = (size_t)len + (size_t)prefix_len + (size_t)zeroes;

    // width includes the sign and 0x prefix, not just the digits
    int padding = _padding_for_width(spec.width, field_len);
    int width_padding = padding;

    if (!(spec.flags & FLAGS_MINUS) && !(spec.flags & FLAGS_ZERO)) {
        while (padding-- > 0) {
            _buf_putc(out, ' ');
        }
    }

    _append_num_prefix(out, number, &spec);

    if (!(spec.flags & FLAGS_MINUS) && (spec.flags & FLAGS_ZERO)) {
        while (padding-- > 0) {
            _buf_putc(out, '0');
        }
    }

    while (zeroes-- > 0) {
        _buf_putc(out, '0');
    }

    if (spec.uppercase) {
        for (int j = 0; j < len; j++) {
            num_buffer[j] = (char)toupper((unsigned char)num_buffer[j]);
        }
    }

    int tail_padding = (spec.flags & FLAGS_MINUS) ? width_padding : 0;
    spec.precision = -1;
    _string_to_buffer(out, num_buffer, &spec, &tail_padding);
}

int vsnprintf(char *restrict buffer, size_t max_size, const char *restrict format, va_list vlist) {
    if (!format) {
        return 0;
    }

    va_list args;
    va_copy(args, vlist);

    size_t written = 0;
    fmt_out_t out = {
        .buf = buffer,
        .cap = max_size,
        .written = &written,
    };

    for (size_t i = 0; format[i]; i++) {
        if (format[i] != '%') {
            _buf_putc(&out, format[i]);
            continue;
        }

        i++;
        if (!format[i]) {
            _buf_putc(&out, '%');
            break;
        }

        fmt_spec_t spec = {
            .flags = _get_flags(format, &i),
            .width = _get_width(format, &i, &args),
            .precision = _get_precision(format, &i, &args),
            .size = _get_size(format, &i),
        };

        if (!format[i]) {
            break;
        }

        spec.base = _get_base(format[i]);
        spec.uppercase = format[i] == 'X';

        if (spec.base == BASE_UNKNOWN) {
            _buf_putc(&out, format[i]);
            continue;
        }

        if (spec.width < 0) {
            spec.flags |= FLAGS_MINUS;
            spec.width = -spec.width;
        }
        if (spec.flags & FLAGS_MINUS) {
            spec.flags &= ~FLAGS_ZERO;
        }

        if (spec.precision < 0) {
            spec.precision = -1;
        }

        if (spec.base == BASE_SDEC) {
            spec.size = -spec.size;
            spec.base = -spec.base;
        }

        if (spec.base == BASE_PTR) {
            spec.size = SIZE_PTR;
            spec.base = -spec.base;
            spec.flags |= FLAGS_HASH;
        }

        if (spec.base < 0) {
            _format_text(&out, spec, &args);
        } else {
            _format_number(&out, spec, &args);
        }
    }

    if (buffer && max_size) {
        size_t term = written < max_size ? written : max_size - 1;
        buffer[term] = '\0';
    }

    va_end(args);

    if (written > (size_t)INT_MAX) {
        return INT_MAX;
    }

    return (int)written;
}
