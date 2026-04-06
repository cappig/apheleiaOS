#include <ctype.h>
#include <errno.h>
#include <float.h>
#include <stdbool.h>
#include <stdlib.h>

static long double _ld_abs(long double value) {
    return value < 0.0L ? -value : value;
}

long double strtold(char const *restrict str, char **restrict endptr) {
    if (!str) {
        errno = EINVAL;
        if (endptr) {
            *endptr = NULL;
        }
        return 0.0L;
    }

    const char *cursor = str;
    while (isspace((unsigned char)*cursor)) {
        cursor++;
    }

    int sign = 1;
    if (*cursor == '+' || *cursor == '-') {
        if (*cursor == '-') {
            sign = -1;
        }
        cursor++;
    }

    long double value = 0.0L;
    bool has_digits = false;

    while (isdigit((unsigned char)*cursor)) {
        has_digits = true;
        value = value * 10.0L + (long double)(*cursor - '0');
        cursor++;
    }

    if (*cursor == '.') {
        long double scale = 0.1L;
        cursor++;

        while (isdigit((unsigned char)*cursor)) {
            has_digits = true;
            value += (long double)(*cursor - '0') * scale;
            scale *= 0.1L;
            cursor++;
        }
    }

    if (!has_digits) {
        if (endptr) {
            *endptr = (char *)str;
        }
        return 0.0L;
    }

    int exp_sign = 1;
    int exp_value = 0;

    const char *exp_mark = cursor;
    if (*cursor == 'e' || *cursor == 'E') {
        cursor++;

        if (*cursor == '+' || *cursor == '-') {
            if (*cursor == '-') {
                exp_sign = -1;
            }
            cursor++;
        }

        const char *exp_start = cursor;
        while (isdigit((unsigned char)*cursor)) {
            if (exp_value < 1000000) {
                exp_value = exp_value * 10 + (*cursor - '0');
            }
            cursor++;
        }

        if (cursor == exp_start) {
            cursor = exp_mark;
            exp_value = 0;
            exp_sign = 1;
        }
    }

    int exponent = exp_sign * exp_value;
    bool overflow = false;
    bool underflow = false;

    if (exponent > 0) {
        for (int i = 0; i < exponent; i++) {
            if (value > LDBL_MAX / 10.0L) {
                value = LDBL_MAX;
                overflow = true;
                break;
            }

            value *= 10.0L;
        }
    } else if (exponent < 0) {
        for (int i = exponent; i < 0; i++) {
            value /= 10.0L;
            if (value != 0.0L && _ld_abs(value) < LDBL_MIN) {
                underflow = true;
            }
        }
    }

    if (overflow || underflow) {
        errno = ERANGE;
    }

    if (endptr) {
        *endptr = (char *)cursor;
    }

    return sign < 0 ? -value : value;
}

double strtod(char const *restrict str, char **restrict endptr) {
    long double value = strtold(str, endptr);
    long double abs_value = _ld_abs(value);

    if (abs_value > (long double)DBL_MAX) {
        errno = ERANGE;
        return value < 0 ? -DBL_MAX : DBL_MAX;
    }

    if (abs_value != 0.0L && abs_value < (long double)DBL_MIN) {
        errno = ERANGE;
    }

    return (double)value;
}

float strtof(char const *restrict str, char **restrict endptr) {
    long double value = strtold(str, endptr);
    long double abs_value = _ld_abs(value);

    if (abs_value > (long double)FLT_MAX) {
        errno = ERANGE;
        return value < 0 ? -FLT_MAX : FLT_MAX;
    }

    if (abs_value != 0.0L && abs_value < (long double)FLT_MIN) {
        errno = ERANGE;
    }

    return (float)value;
}

double atof(char const *str) {
    return strtod(str, NULL);
}
