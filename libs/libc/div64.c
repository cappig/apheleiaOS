#include <base/types.h>

#if defined(__has_attribute)
#if __has_attribute(weak)
#define WEAK_ATTR __attribute__((weak))
#endif
#endif
#ifndef WEAK_ATTR
#define WEAK_ATTR
#endif

static u64 udivmod64(u64 n, u64 d, u64 *rem) {
    if (d == 0) {
        if (rem) {
            *rem = 0;
        }
        return 0;
    }

    u64 q = 0;
    u64 r = 0;

    for (int i = 63; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1ULL);
        if (r >= d) {
            r -= d;
            q |= (1ULL << i);
        }
    }

    if (rem) {
        *rem = r;
    }

    return q;
}

WEAK_ATTR u64 __udivdi3(u64 n, u64 d) {
    return udivmod64(n, d, 0);
}

WEAK_ATTR u64 __umoddi3(u64 n, u64 d) {
    u64 r = 0;
    udivmod64(n, d, &r);
    return r;
}

WEAK_ATTR i64 __divdi3(i64 n, i64 d) {
    int neg = 0;
    u64 un = (u64)n;
    u64 ud = (u64)d;

    if (n < 0) {
        neg ^= 1;
        un = (u64)(-n);
    }
    if (d < 0) {
        neg ^= 1;
        ud = (u64)(-d);
    }

    u64 q = udivmod64(un, ud, 0);
    return neg ? -(i64)q : (i64)q;
}

WEAK_ATTR i64 __moddi3(i64 n, i64 d) {
    int neg = 0;
    u64 un = (u64)n;
    u64 ud = (u64)d;

    if (n < 0) {
        neg = 1;
        un = (u64)(-n);
    }
    if (d < 0) {
        ud = (u64)(-d);
    }

    u64 r = 0;
    udivmod64(un, ud, &r);
    return neg ? -(i64)r : (i64)r;
}

WEAK_ATTR u64 __udivmoddi4(u64 n, u64 d, u64 *rem) {
    return udivmod64(n, d, rem);
}

WEAK_ATTR i64 __divmoddi4(i64 n, i64 d, i64 *rem) {
    int neg = 0;
    u64 un = (u64)n;
    u64 ud = (u64)d;

    if (n < 0) {
        neg ^= 1;
        un = (u64)(-n);
    }
    if (d < 0) {
        neg ^= 1;
        ud = (u64)(-d);
    }

    u64 r = 0;
    u64 q = udivmod64(un, ud, &r);

    if (rem) {
        i64 rr = (i64)r;
        if (n < 0) {
            rr = -rr;
        }
        *rem = rr;
    }

    return neg ? -(i64)q : (i64)q;
}
