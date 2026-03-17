#include "stdlib.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(__has_attribute)
#if __has_attribute(nonstring)
#define NONSTRING_ATTR __attribute__((nonstring))
#endif
#endif
#ifndef NONSTRING_ATTR
#define NONSTRING_ATTR
#endif

static const char digits[36] NONSTRING_ATTR =
    "0123456789abcdefghijklmnopqrstuvwxyz";


size_t ulltoa(unsigned long long value, char *buf, int base) {
    if (base < 2 || base > 36) {
        return 0;
    }

    char buffer[sizeof(value) * CHAR_BIT + 1 + 1];

    char *pos = &buffer[sizeof(buffer) - 1];
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

size_t ultoa(unsigned long value, char *buf, int base) {
    return ulltoa((unsigned long long)value, buf, base);
}

size_t uitoa(unsigned int value, char *buf, int base) {
    return ulltoa((unsigned long long)value, buf, base);
}


size_t lltoa(long long value, char *buf, int base) {
    unsigned long long neg_val;

    if (value < 0) {
        *buf++ = '-';
        neg_val = -value;
    } else {
        neg_val = value;
    }

    return ulltoa(neg_val, buf, base) + (value < 0);
}

size_t ltoa(long value, char *buf, int base) {
    return lltoa((long long)value, buf, base);
}

size_t itoa(int value, char *buf, int base) {
    return lltoa((long long)value, buf, base);
}


lldiv_t ulldiv(unsigned long long num, unsigned long den) {
    lldiv_t ret = {0};

    ret.rem = num % den;
    ret.quot = num / den;

    return ret;
}

int bcdtoi(int bcd) {
    return (bcd & 0x0f) + (bcd >> 4) * 10;
}

int itobcd(int num) {
    return ((num / 10) << 4) + num % 10;
}


unsigned short bswaps(unsigned short num) {
    uint16_t v = (uint16_t)num;
    v = (uint16_t)((v << 8) | (v >> 8));
    return (unsigned short)v;
}

unsigned long bswapl(unsigned long num) {
    uint32_t v = (uint32_t)num;
    v = ((v & 0x000000ffU) << 24) |
        ((v & 0x0000ff00U) << 8) |
        ((v & 0x00ff0000U) >> 8) |
        ((v & 0xff000000U) >> 24);
    return (unsigned long)v;
}

unsigned long long bswapll(unsigned long long num) {
    uint64_t v = (uint64_t)num;
    v = ((v & 0x00000000000000ffULL) << 56) |
        ((v & 0x000000000000ff00ULL) << 40) |
        ((v & 0x0000000000ff0000ULL) << 24) |
        ((v & 0x00000000ff000000ULL) << 8) |
        ((v & 0x000000ff00000000ULL) >> 8) |
        ((v & 0x0000ff0000000000ULL) >> 24) |
        ((v & 0x00ff000000000000ULL) >> 40) |
        ((v & 0xff00000000000000ULL) >> 56);
    return (unsigned long long)v;
}
