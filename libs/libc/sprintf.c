#include "ctype.h"
#include "stdarg.h"
#include "stddef.h"
#include "stdint.h"
#include "stdlib.h"
#include "string.h"


#define FLAGS_MINUS (1 << 0)
#define FLAGS_PLUS  (1 << 1)
#define FLAGS_SPACE (1 << 2)
#define FLAGS_HASH  (1 << 3)
#define FLAGS_ZERO  (1 << 4)

static int _get_flags(const char* format, size_t* index) {
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

    if ((flags & FLAGS_SPACE) && (flags & FLAGS_PLUS))
        flags &= ~FLAGS_SPACE;

    return flags;
}

static int _get_width(const char* format, size_t* index, va_list vlist) {
    int width = 0;

    if (isdigit(format[*index])) {
        char* end;
        width = strtol(&format[*index], &end, 10);

        *index += (size_t)(end - &format[*index]);
    } else if (format[*index] == '*') {
        width = abs(va_arg(vlist, int));

        *index += 1;
    }

    return width;
}

static int _get_precision(const char* format, size_t* index, va_list vlist) {
    int precision = -1;

    if (format[*index] != '.')
        return precision;

    (*index)++;

    if (isdigit(format[*index])) {
        char* end;
        precision = strtol(&format[*index], &end, 10);

        *index += (size_t)(end - &format[*index]);
    } else if (format[*index] == '*') {
        precision = va_arg(vlist, int);

        *index += 1;
    }

    return precision;
}

// Negative values represent signed types
#define SIZE_INT   1
#define SIZE_CHAR  2
#define SIZE_SHORT 3
#define SIZE_LONG  4
#define SIZE_LLONG 5

#define type_size(type) ((sizeof(type) == sizeof(long)) ? SIZE_LONG : SIZE_LLONG)

static int _get_size(const char* format, size_t* index) {
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
        size = type_size(size_t);
        break;
    case 'j':
        size = type_size(intmax_t);
        break;
    case 't':
        size = type_size(ptrdiff_t);
        break;
    default:
        (*index)--;
        break;
    }

    return size;
}

static long long _get_var_number(int size, va_list vlist) {
    long long number;

    switch (size) {
    case SIZE_LLONG:
        number = va_arg(vlist, unsigned long long);
        break;
    case -SIZE_LLONG:
        number = va_arg(vlist, long long);
        break;

    case SIZE_LONG:
        number = va_arg(vlist, unsigned long);
        break;
    case -SIZE_LONG:
        number = va_arg(vlist, long);
        break;

    default:
    case SIZE_INT:
    case SIZE_SHORT:
    case SIZE_CHAR:
        number = va_arg(vlist, unsigned int);
        break;

    case -SIZE_INT:
    case -SIZE_SHORT:
    case -SIZE_CHAR:
        number = va_arg(vlist, int);
        break;
    }

    return number;
}

static size_t _string_to_buffer(char* buffer, char* string, int flags, int precision, int* padding) {
    const char* buf_start = buffer;

    if (!(flags & FLAGS_MINUS) && !(flags & FLAGS_ZERO))
        while ((*padding)-- > 0)
            *buffer++ = ' ';

    while (*string && (precision < 0 || precision--))
        *buffer++ = *string++;

    if ((flags & FLAGS_MINUS) && !(flags & FLAGS_ZERO))
        while ((*padding)-- > 0)
            *buffer++ = ' ';

    return (size_t)(buffer - buf_start);
}

static size_t
_append_num_prefix(char* buffer, long long number, int flags, int base, int size, int* padding) {
    const char* buf_start = buffer;

    if (!(flags & FLAGS_MINUS) && !(flags & FLAGS_ZERO))
        while ((*padding)-- > 0)
            *buffer++ = ' ';

    char sign = 0;
    if (size < 0 && number < 0)
        sign = '-';
    else if (flags & FLAGS_PLUS)
        sign = '+';
    else if (flags & FLAGS_SPACE)
        sign = ' ';

    if (sign) {
        *buffer++ = sign;
        (*padding)--;
    }

    char* prefix = "";
    if (base == 2)
        prefix = "0b";
    else if (base == 8)
        prefix = "0";
    else if (base == 16)
        prefix = "0x";

    while (*prefix && (flags & FLAGS_HASH))
        *buffer++ = *prefix++;

    if (flags & FLAGS_ZERO)
        while ((*padding)-- > 0)
            *buffer++ = '0';

    return (size_t)(buffer - buf_start);
}

#define BASE_STRING -1
#define BASE_CHAR   -2
#define BASE_OTHER  -3

static int _get_base(char type) {
    switch (type) {
    case 'p':
    case 'x':
    case 'X':
        return 16;
    case 'i':
    case 'd':
        return -10;
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
    default:
        return BASE_OTHER;
    }
}


int vsnprintf(char* restrict buffer, size_t max_size, const char* restrict format, va_list vlist) {
    if (!buffer)
        return 0;

    int printed = 0;

    for (size_t i = 0; format[i] && i < max_size; i++) {
        if (format[i] != '%') {
            buffer[printed++] = format[i];
            continue;
        }

        i++;

        int flags = _get_flags(format, &i);
        int width = _get_width(format, &i, vlist);
        int precision = _get_precision(format, &i, vlist);
        int size = _get_size(format, &i);

        int base = _get_base(format[i]);

        if (base == -10) {
            size = -size;
            base = -base;
        }

        if (base == BASE_OTHER) {
            buffer[printed++] = format[i];
            continue;
        }

        if (base < 0) {
            char char_holder[2] = {0};
            char* string;

            if (base == BASE_STRING) {
                string = va_arg(vlist, char*);

                if (!string)
                    string = "(null)";
            }
            // A tiny hack that allows us to reuse the code bellow :^)
            else {
                char_holder[0] = (char)va_arg(vlist, int);
                string = char_holder;
            }

            int len = strlen(string);
            int padding = (width > len) ? width - len : 0;

            printed += _string_to_buffer(&buffer[printed], string, flags, precision, &padding);
        } else {
            long long number = _get_var_number(size, vlist);
            bool negative = (size < 0 && number < 0);

            if (precision > 0) {
                flags |= FLAGS_ZERO;
                width = (precision > width) ? precision : width;
            }

            char num_buffer[66] = {0};
            int len = ulltoa(negative ? -number : number, num_buffer, base);

            int padding = (width > len) ? width - len : 0;

            printed += _append_num_prefix(&buffer[printed], number, flags, base, size, &padding);

            printed += _string_to_buffer(&buffer[printed], num_buffer, flags, precision, &padding);
        }
    }

    return printed;
}
