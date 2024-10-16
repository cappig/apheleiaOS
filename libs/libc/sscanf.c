#include "ctype.h"
#include "stdarg.h"
#include "stdbool.h"
#include "stddef.h"
#include "stdlib.h"


static int _get_width(const char* format, size_t* index) {
    int width = -1;

    if (isdigit(format[*index])) {
        char* end;
        width = strtol(&format[*index], &end, 10);

        *index += (size_t)(end - &format[*index]);
    }

    return width;
}

static void _consume_size(const char* format, size_t* index) {
    switch (format[*index]) {
    case 'h':
        (*index)++;
        if (format[*index] == 'h')
            (*index)++;
        break;

    case 'l':
        (*index)++;
        if (format[*index] == 'l')
            (*index)++;
        break;

    case 'z':
    case 'j':
    case 't':
        (*index)++;
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

static void _adjust_width(int base, const char* restrict str, int* width) {
    if (base == BASE_STRING) {
        int len = 0;
        while (!isspace(str[len]) && str[len])
            len++;

        if (*width == -1)
            *width = len;
        else
            *width = min(*width, len);

    } else if (base == BASE_CHAR) {
        if (*width == -1)
            *width = 1;
    }
}


int vsnscanf(const char* restrict str, size_t max, const char* restrict format, va_list vlist) {
    size_t filled = 0;
    size_t j = 0;

    for (size_t i = 0; format[i] && i < max; i++) {
        if (format[i] == '%') {
            i++;

            bool ignore = false;
            if (format[i] == '*') {
                ignore = true;
                i++;
            }

            int width = _get_width(format, &i);

            _consume_size(format, &i);

            int base = _get_base(format[i]);

            // Number
            if (base > 0) {
                char* end;
                unsigned long long number = strtoll(&str[j], &end, base);

                j += (size_t)(end - &str[j]);

                if (ignore)
                    continue;

                unsigned long long* ptr = va_arg(vlist, void*);
                *ptr = number;
                filled++;
            }
            // String
            else if (base == BASE_CHAR || base == BASE_STRING) {
                _adjust_width(base, &str[j], &width);

                if (ignore) {
                    j += width;
                    continue;
                }

                char* ptr = va_arg(vlist, char*);
                filled++;

                for (int c = 0; c < width; c++)
                    ptr[c] = str[j++];

                if (base == BASE_STRING)
                    ptr[j] = '\0';
            }
        }
        // Handle spaces
        else if (format[i] == ' ') {
            while (isspace(str[j]) && str[j])
                j++;
        }
        // Check str against format
        else {
            if (format[i] != str[j])
                break;

            j++;
        }
    }

    return filled;
}
