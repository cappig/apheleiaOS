#include "stdlib.h"

#include <stddef.h>

#include "ctype.h"
#include "errno.h"
#include "limits.h"
#include "stdbool.h"
#include "string.h"


static const char *_skip_space(const char *str) {
    const char *cursor = str;
    while (cursor && isspace((unsigned char)*cursor)) {
        cursor++;
    }
    return cursor;
}

static int _digit_value(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'z') {
        return (ch - 'a') + 10;
    }
    if (ch >= 'A' && ch <= 'Z') {
        return (ch - 'A') + 10;
    }
    return -1;
}

static int _normalize_base(const char **cursor, int base) {
    if (!cursor || !*cursor) {
        return -1;
    }

    if (base < 0 || base == 1 || base > 36) {
        return -1;
    }

    const char *s = *cursor;

    if (!base) {
        if (s[0] == '0') {
            if (
                (s[1] == 'x' || s[1] == 'X') &&
                _digit_value(s[2]) >= 0 &&
                _digit_value(s[2]) < 16
            ) {
                *cursor = s + 2;
                return 16;
            }

            return 8;
        }

        return 10;
    }

    if (
        base == 16 &&
        s[0] == '0' &&
        (s[1] == 'x' || s[1] == 'X') &&
        _digit_value(s[2]) >= 0 &&
        _digit_value(s[2]) < 16
    ) {
        *cursor = s + 2;
    }

    return base;
}

static unsigned long long _parse_unsigned(
    const char *str,
    char **endptr,
    int base,
    bool *negative_out,
    bool *has_digits_out,
    bool *overflow_out
) {
    bool negative = false;
    bool has_digits = false;
    bool overflow = false;

    const char *cursor = _skip_space(str);

    if (*cursor == '+' || *cursor == '-') {
        negative = (*cursor == '-');
        cursor++;
    }

    int parsed_base = _normalize_base(&cursor, base);
    if (parsed_base < 0) {
        errno = EINVAL;
        if (endptr) {
            *endptr = (char *)str;
        }
        if (negative_out) {
            *negative_out = false;
        }
        if (has_digits_out) {
            *has_digits_out = false;
        }
        if (overflow_out) {
            *overflow_out = false;
        }
        return 0;
    }

    unsigned long long value = 0;
    unsigned long long cutoff = ULLONG_MAX / (unsigned long long)parsed_base;
    unsigned long long cutlim = ULLONG_MAX % (unsigned long long)parsed_base;
    const char *digits_start = cursor;

    for (;;) {
        int digit = _digit_value(*cursor);
        if (digit < 0 || digit >= parsed_base) {
            break;
        }

        has_digits = true;

        if (
            value > cutoff ||
            (value == cutoff && (unsigned long long)digit > cutlim)
        ) {
            overflow = true;
        } else {
            value = value * (unsigned long long)parsed_base + (unsigned long long)digit;
        }

        cursor++;
    }

    if (!has_digits) {
        cursor = str;
    }

    if (endptr) {
        *endptr = (char *)(has_digits ? cursor : str);
    }

    if (negative_out) {
        *negative_out = negative;
    }
    if (has_digits_out) {
        *has_digits_out = has_digits;
    }
    if (overflow_out) {
        *overflow_out = overflow;
    }

    (void)digits_start;
    return value;
}


long long strtoll(char const *restrict str, char **restrict endptr, int base) {
    bool negative = false;
    bool has_digits = false;
    bool overflow = false;
    unsigned long long magnitude = _parse_unsigned(
        str, endptr, base, &negative, &has_digits, &overflow
    );

    if (!has_digits) {
        return 0;
    }

    unsigned long long limit =
        negative ? (unsigned long long)LLONG_MAX + 1ULL : (unsigned long long)LLONG_MAX;

    if (overflow || magnitude > limit) {
        errno = ERANGE;
        return negative ? LLONG_MIN : LLONG_MAX;
    }

    if (negative) {
        if (magnitude == (unsigned long long)LLONG_MAX + 1ULL) {
            return LLONG_MIN;
        }
        return -(long long)magnitude;
    }

    return (long long)magnitude;
}

long strtol(char const *restrict str, char **restrict endptr, int base) {
    long long value = strtoll(str, endptr, base);

    if (value > (long long)LONG_MAX) {
        errno = ERANGE;
        return LONG_MAX;
    }
    if (value < (long long)LONG_MIN) {
        errno = ERANGE;
        return LONG_MIN;
    }

    return (long)value;
}

unsigned long long
strtoull(char const *restrict str, char **restrict endptr, int base) {
    bool negative = false;
    bool has_digits = false;
    bool overflow = false;
    unsigned long long value = _parse_unsigned(
        str, endptr, base, &negative, &has_digits, &overflow
    );

    if (!has_digits) {
        return 0;
    }

    if (overflow) {
        errno = ERANGE;
        return ULLONG_MAX;
    }

    if (negative) {
        return 0ULL - value;
    }

    return value;
}

unsigned long
strtoul(char const *restrict str, char **restrict endptr, int base) {
    unsigned long long value = strtoull(str, endptr, base);

    if (value > (unsigned long long)ULONG_MAX) {
        errno = ERANGE;
        return ULONG_MAX;
    }

    return (unsigned long)value;
}


long long atoll(char const *str) {
    return strtoll(str, NULL, 10);
}

long atol(char const *str) {
    return (long)strtoll(str, NULL, 10);
}

int atoi(char const *str) {
    return (int)strtoll(str, NULL, 10);
}


long long llabs(long long n) {
    return (n < 0) ? -n : n;
}

long labs(long n) {
    return llabs(n);
}

int abs(int n) {
    return llabs(n);
}

div_t div(int numer, int denom) {
    div_t out = {0, 0};
    if (!denom) {
        return out;
    }

    out.quot = numer / denom;
    out.rem = numer % denom;
    return out;
}

ldiv_t ldiv(long numer, long denom) {
    ldiv_t out = {0, 0};
    if (!denom) {
        return out;
    }

    out.quot = numer / denom;
    out.rem = numer % denom;
    return out;
}

lldiv_t lldiv(long long numer, long long denom) {
    lldiv_t out = {0, 0};
    if (!denom) {
        return out;
    }

    out.quot = numer / denom;
    out.rem = numer % denom;
    return out;
}

void *bsearch(
    const void *key,
    const void *base,
    size_t nmemb,
    size_t size,
    int (*compar)(const void *, const void *)
) {
    if (!key || !base || !compar || !size) {
        return NULL;
    }

    size_t left = 0;
    size_t right = nmemb;

    while (left < right) {
        size_t mid = left + (right - left) / 2;
        const unsigned char *elem =
            (const unsigned char *)base + (mid * size);
        int cmp = compar(key, elem);

        if (!cmp) {
            return (void *)elem;
        }

        if (cmp < 0) {
            right = mid;
        } else {
            left = mid + 1;
        }
    }

    return NULL;
}


#ifdef EXTERNAL_ALLOC
static libc_alloc_ops_t alloc_ops = {0};

void __libc_init_alloc(const libc_alloc_ops_t *ops) {
    if (!ops) {
        alloc_ops.malloc_fn = NULL;
        alloc_ops.free_fn = NULL;
        return;
    }

    alloc_ops = *ops;
}

void *malloc(size_t size) {
    if (!size)
        return NULL;

    if (!alloc_ops.malloc_fn)
        return NULL;

    return alloc_ops.malloc_fn(size);
}

void *calloc(size_t num, size_t size) {
    if (!num)
        return NULL;

    void *ret = malloc(size * num);

    if (ret)
        memset(ret, 0, size * num);

    return ret;
}

void free(void *ptr) {
    if (!ptr)
        return;

    if (!alloc_ops.free_fn)
        return;

    alloc_ops.free_fn(ptr);
}
#endif
