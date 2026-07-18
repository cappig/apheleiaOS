#include <base/types.h>

#if defined(__has_attribute)
#if __has_attribute(weak)
#define WEAK_ATTR __attribute__((weak))
#endif
#if __has_attribute(noinline)
#define NOINLINE_ATTR __attribute__((noinline))
#endif
#endif
#ifndef WEAK_ATTR
#define WEAK_ATTR
#endif
#ifndef NOINLINE_ATTR
#define NOINLINE_ATTR
#endif

// At -O2, inlining this helper into every ABI wrapper adds several KiB.
static NOINLINE_ATTR u64 udivmod64(u64 n, u64 d, u64 *rem) {
    if (!d) {
        if (rem) {
            *rem = 0;
        }
        return 0;
    }

    if (n < d) {
        if (rem) {
            *rem = n;
        }
        return 0;
    }

    if (d == 1) {
        if (rem) {
            *rem = 0;
        }
        return n;
    }

    if (!(d & (d - 1))) {
        unsigned int shift = (unsigned int)__builtin_ctzll(d);
        if (rem) {
            *rem = n & (d - 1);
        }
        return n >> shift;
    }

    if (n <= UINT32_MAX) {
        u32 n32 = (u32)n;
        u32 d32 = (u32)d;
        u32 q32 = n32 / d32;

        if (rem) {
            *rem = n32 - q32 * d32;
        }
        return q32;
    }

    unsigned int shift = (unsigned int)(__builtin_clzll(d) - __builtin_clzll(n));
    u64 divisor = d << shift;
    u64 bit = 1ULL << shift;
    u64 q = 0;

    do {
        if (n >= divisor) {
            n -= divisor;
            q |= bit;
        }

        divisor >>= 1;
        bit >>= 1;
    } while (bit);

    if (rem) {
        *rem = n;
    }

    return q;
}

static u64 i64_magnitude(i64 value) {
    u64 bits = (u64)value;
    return value < 0 ? 0 - bits : bits;
}

static i64 i64_sign(u64 value, bool negative) {
    return (i64)(negative ? 0 - value : value);
}

WEAK_ATTR u64 __udivdi3(u64 n, u64 d) {
    return udivmod64(n, d, NULL);
}

WEAK_ATTR u64 __umoddi3(u64 n, u64 d) {
    u64 r = 0;
    udivmod64(n, d, &r);
    return r;
}

WEAK_ATTR i64 __divdi3(i64 n, i64 d) {
    bool negative = (n < 0) != (d < 0);
    u64 q = udivmod64(i64_magnitude(n), i64_magnitude(d), NULL);
    return i64_sign(q, negative);
}

WEAK_ATTR i64 __moddi3(i64 n, i64 d) {
    u64 r = 0;
    udivmod64(i64_magnitude(n), i64_magnitude(d), &r);
    return i64_sign(r, n < 0);
}

WEAK_ATTR u64 __udivmoddi4(u64 n, u64 d, u64 *rem) {
    return udivmod64(n, d, rem);
}

WEAK_ATTR i64 __divmoddi4(i64 n, i64 d, i64 *rem) {
    bool negative = (n < 0) != (d < 0);
    u64 r = 0;
    u64 q = udivmod64(i64_magnitude(n), i64_magnitude(d), &r);

    if (rem) {
        *rem = i64_sign(r, n < 0);
    }

    return i64_sign(q, negative);
}
