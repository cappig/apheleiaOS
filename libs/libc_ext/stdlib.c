#include "stdlib.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

static const char digits[36] = "0123456789abcdefghijklmnopqrstuvwxyz";


size_t ulltoa(unsigned long long value, char* buf, int base) {
    if (base < 2 || base > 36)
        return 0;

    char buffer[sizeof(value) * CHAR_BIT + 1 + 1];

    char* pos = &buffer[sizeof(buffer) - 1];
    *pos = '\0';

    do {
        lldiv_t div = ulldiv(value, base);

        *(--pos) = digits[div.rem];

        value = div.quot;
    } while (value);

    size_t size_used = &buffer[sizeof(buffer)] - pos;

    memcpy(buf, pos, size_used);

    return size_used - 1;
}

size_t ultoa(unsigned long value, char* buf, int base) {
    return ulltoa((unsigned long long)value, buf, base);
}

size_t uitoa(unsigned int value, char* buf, int base) {
    return ulltoa((unsigned long long)value, buf, base);
}


size_t lltoa(long long value, char* buf, int base) {
    unsigned long long neg_val;

    if (value < 0) {
        *buf++ = '-';
        neg_val = -value;
    } else {
        neg_val = value;
    }

    return ulltoa(neg_val, buf, base) + (value < 0);
}

size_t ltoa(long value, char* buf, int base) {
    return lltoa((long long)value, buf, base);
}

size_t itoa(int value, char* buf, int base) {
    return lltoa((long long)value, buf, base);
}


lldiv_t ulldiv(unsigned long long num, unsigned long den) {
    lldiv_t ret = {0};

#if defined(__x86_64__)
    ret.rem = num % den;
    ret.quot = num / den;
#else
    unsigned long high = num >> 32;
    unsigned long low = num & 0xffffffff;

    unsigned long n_high = high / den;
    high %= den;

    asm volatile("divl %2" : "+a"(low), "+d"(high) : "r"(den));

    ret.rem = high;
    ret.quot = ((long long)n_high << 32 | low);
#endif

    return ret;
}
