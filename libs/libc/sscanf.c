#include "ctype.h"
#include "stdarg.h"
#include "stdbool.h"
#include "stddef.h"
#include "stdint.h"
#include "stdlib.h"

static int _get_width(const char *format, size_t *index) {
    int width = -1;

    if (isdigit(format[*index])) {
        char *end;
        width = strtol(&format[*index], &end, 10);

        *index += (size_t)(end - &format[*index]);
    }

    return width;
}

enum scan_size {
    SCAN_CHAR,
    SCAN_SHORT,
    SCAN_INT,
    SCAN_LONG,
    SCAN_LLONG,
    SCAN_SIZE,
    SCAN_PTRDIFF,
    SCAN_INTMAX,
    SCAN_PTR
};

static enum scan_size _read_size(const char *format, size_t *index) {
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

    case 'j':
        (*index)++;
        return SCAN_INTMAX;

    case 't':
        (*index)++;
        return SCAN_PTRDIFF;

    default:
        return SCAN_INT;
    }
}

static bool _is_signed_number(char type) {
    return type == 'd' || type == 'i';
}

static void _store_signed(void *ptr, enum scan_size size, long long value) {
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
        *(size_t *)ptr = (size_t)value;
        break;

    case SCAN_PTRDIFF:
        *(ptrdiff_t *)ptr = (ptrdiff_t)value;
        break;

    case SCAN_INTMAX:
        *(intmax_t *)ptr = (intmax_t)value;
        break;

    case SCAN_INT:
    default:
        *(int *)ptr = (int)value;
        break;
    }
}

static void _store_unsigned(void *ptr, enum scan_size size, unsigned long long value) {
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
        *(ptrdiff_t *)ptr = (ptrdiff_t)value;
        break;

    case SCAN_INTMAX:
        *(uintmax_t *)ptr = (uintmax_t)value;
        break;

    case SCAN_PTR:
        *(void **)ptr = (void *)(uintptr_t)value;
        break;

    case SCAN_INT:
    default:
        *(unsigned int *)ptr = (unsigned int)value;
        break;
    }
}

#define BASE_STRING  -1
#define BASE_CHAR    -2
#define BASE_SET     -3
#define BASE_UNKNOWN -4

static int _get_base(char type) {
    switch (type) {
    case 'p':
    case 'x':
    case 'X':
        return 16;

    case 'i':
        return 0;

    case 'd':
    case 'u':
        return 10;

    case 'o':
        return 8;

    case 'b':
        return 2;

    case 'c':
        return BASE_CHAR;

    case 's':
        return BASE_STRING;

    case '[':
        return BASE_SET;

    default:
        return BASE_UNKNOWN;
    }
}

static void _adjust_width(int base, const char *restrict str, int *width) {
    if (base == BASE_STRING) {
        int len = 0;
        while (!isspace(str[len]) && str[len]) {
            len++;
        }

        if (*width == -1) {
            *width = len;
        } else {
            *width = min(*width, len);
        }

    } else if (base == BASE_CHAR) {
        if (*width == -1) {
            *width = 1;
        }
    }
}

int vsnscanf(const char *restrict str, size_t max, const char *restrict format, va_list vlist) {
    size_t filled = 0;
    size_t j = 0;

    for (size_t i = 0; format[i] && filled < max; i++) {
        if (format[i] == '%') {
            i++;

            bool ignore = false;
            if (format[i] == '*') {
                ignore = true;
                i++;
            }

            int width = _get_width(format, &i);

            enum scan_size size = _read_size(format, &i);

            int base = _get_base(format[i]);
            if (format[i] == 'p') {
                size = SCAN_PTR;
            }

            // number
            if (base >= 0) {
                char *end;
                const char *start = &str[j];

                if (_is_signed_number(format[i])) {
                    long long number = strtoll(start, &end, base);
                    if (end == start) {
                        break;
                    }

                    j += (size_t)(end - start);

                    if (ignore) {
                        continue;
                    }

                    _store_signed(va_arg(vlist, void *), size, number);
                    filled++;
                    continue;
                }

                unsigned long long number = strtoull(start, &end, base);
                if (end == start) {
                    break;
                }

                j += (size_t)(end - start);

                if (ignore) {
                    continue;
                }

                _store_unsigned(va_arg(vlist, void *), size, number);
                filled++;
            }
            // string
            else if (base == BASE_CHAR || base == BASE_STRING) {
                _adjust_width(base, &str[j], &width);

                if (ignore) {
                    j += width;
                    continue;
                }

                char *ptr = va_arg(vlist, char *);
                filled++;

                for (int c = 0; c < width; c++) {
                    ptr[c] = str[j++];
                }

                if (base == BASE_STRING) {
                    ptr[width] = '\0';
                }
            }
        }
        // handle spaces
        else if (format[i] == ' ') {
            while (isspace(str[j]) && str[j]) {
                j++;
            }
        }
        // check str against format
        else {
            if (format[i] != str[j]) {
                break;
            }

            j++;
        }
    }

    return filled;
}
