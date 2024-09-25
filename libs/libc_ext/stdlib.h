#pragma once

#include <stdlib.h>

#include "stddef.h"

size_t uitoa(unsigned int value, char* buf, int base);
size_t ultoa(unsigned long value, char* buf, int base);
size_t ulltoa(unsigned long long value, char* buf, int base);

size_t itoa(int value, char* buf, int base);
size_t ltoa(long value, char* buf, int base);
size_t lltoa(long long value, char* buf, int base);

lldiv_t ulldiv(unsigned long long num, unsigned long den);

int bcdtoi(int bcd);
int itobcd(int num);

unsigned short bswaps(unsigned short num);
unsigned long bswapl(unsigned long num);
unsigned long long bswapll(unsigned long long num);


#define max(a, b)                  \
    ({                             \
        const typeof(a) __a = (a); \
        const typeof(b) __b = (b); \
        __a > __b ? __a : __b;     \
    })

#define min(a, b)                  \
    ({                             \
        const typeof(a) __a = (a); \
        const typeof(b) __b = (b); \
        __a < __b ? __a : __b;     \
    })

#define clamp(num, min, max)                         \
    ({                                               \
        const typeof(x) __x = (num);                 \
        const typeof(min) __l = (min);               \
        const typeof(max) __h = (max);               \
        (__x > __h) ? __h : (__x < __l ? __l : __x); \
    })
