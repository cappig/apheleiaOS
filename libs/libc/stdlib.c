#include "stdlib.h"

#include "ctype.h"
#include "errno.h"
#include "limits.h"

long long strtoll(char const* restrict str, char** restrict endptr, int base) {
    const char* pos = str;
    unsigned long long ret = 0;

    if (base < 0 || base == 1 || base > 36) {
        errno = EINVAL;
        if (endptr)
            *endptr = (char*)str;

        return 0;
    }

    while (isspace(*pos))
        pos++;

    bool negative = false;
    if (*pos == '-') {
        negative = true;
        pos++;
    } else if (*pos == '+') {
        pos++;
    }

    int a = pos[0];
    int b = tolower(pos[1]);

    if ((base == 0 || base == 16) && (a == '0' && b == 'x') && isxdigit(pos[2])) {
        base = 16;
        pos += 2;
    } else if (base == 0 && a == '0') {
        base = 8;
    } else if (base == 0) {
        base = 10;
    }

    unsigned long long cutoff = negative ? -(unsigned long long)LLONG_MIN : LLONG_MAX;

    lldiv_t div = ulldiv(cutoff, base);
    unsigned long cutlim = div.rem;

    bool has_converted = false;
    bool out_of_range = false;
    for (;;) {
        int dig = tolower(*pos++);

        if (isdigit(dig))
            dig -= '0';
        else if (isalpha(dig))
            dig -= 'a' - 10;
        else
            break;

        if (dig >= base)
            break;

        if (out_of_range || ret > cutoff || (ret == cutoff && dig > (int)cutlim)) {
            out_of_range = true;
        } else {
            ret *= base;
            ret += dig;
            has_converted = true;
        }
    }

    if (endptr)
        *endptr = (char*)(has_converted ? pos - 1 : str);

    if (out_of_range) {
        errno = ERANGE;
        return negative ? LLONG_MIN : LLONG_MAX;
    }

    return negative ? -ret : ret;
}

long strtol(char const* restrict str, char** restrict endptr, int base) {
    return (long)strtoll(str, endptr, base);
}


long long atoll(char const* str) {
    return strtoll(str, NULL, 10);
}

long atol(char const* str) {
    return (long)strtoll(str, NULL, 10);
}

int atoi(char const* str) {
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
