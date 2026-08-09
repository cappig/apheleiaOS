#include <base/types.h>

#if defined(__has_attribute)
#if __has_attribute(weak)
#define WEAK_ATTR __attribute__((weak))
#endif
#endif
#ifndef WEAK_ATTR
#define WEAK_ATTR
#endif

static u32 _umul32(u32 lhs, u32 rhs) {
    u32 result = 0;

    while (rhs) {
        if (rhs & 1U) {
            result += lhs;
        }

        lhs <<= 1;
        rhs >>= 1;
    }

    return result;
}

static u64 _umul64(u64 lhs, u64 rhs) {
    // Keep the loop in 32-bit operations. On RV32, a conditional 64-bit add
    // may otherwise be lowered to __muldi3 and recurse back into this helper.
    u32 lhs_lo = (u32)lhs;
    u32 lhs_hi = (u32)(lhs >> 32);
    u32 rhs_lo = (u32)rhs;
    u32 rhs_hi = (u32)(rhs >> 32);
    u32 result_lo = 0;
    u32 result_hi = 0;

    while (rhs_lo || rhs_hi) {
        if (rhs_lo & 1U) {
            u32 old_lo = result_lo;

            result_lo += lhs_lo;
            result_hi += lhs_hi + (result_lo < old_lo);
        }

        lhs_hi = (lhs_hi << 1) | (lhs_lo >> 31);
        lhs_lo <<= 1;
        rhs_lo = (rhs_lo >> 1) | (rhs_hi << 31);
        rhs_hi >>= 1;
    }

    return ((u64)result_hi << 32) | result_lo;
}

#if defined(__SIZEOF_INT128__)
static void _umul128_64(u64 lhs, u64 rhs, u64 *hi_out, u64 *lo_out) {
    u64 lhs_lo = (u32)lhs;
    u64 lhs_hi = lhs >> 32;
    u64 rhs_lo = (u32)rhs;
    u64 rhs_hi = rhs >> 32;

    u64 p0 = _umul64(lhs_lo, rhs_lo);
    u64 p1 = _umul64(lhs_lo, rhs_hi);
    u64 p2 = _umul64(lhs_hi, rhs_lo);
    u64 p3 = _umul64(lhs_hi, rhs_hi);

    u64 carry = (p0 >> 32) + (u32)p1 + (u32)p2;
    u64 lo = (p0 & 0xffffffffULL) | (carry << 32);
    u64 hi = p3 + (p1 >> 32) + (p2 >> 32) + (carry >> 32);

    if (hi_out) {
        *hi_out = hi;
    }

    if (lo_out) {
        *lo_out = lo;
    }
}
#endif

static u32 _udivmod32(u32 num, u32 den, u32 *rem_out) {
    if (!den) {
        if (rem_out) {
            *rem_out = 0;
        }

        return 0;
    }

    u32 quot = 0;
    u32 rem = 0;

    for (int bit = 31; bit >= 0; bit--) {
        rem = (rem << 1) | ((num >> bit) & 1U);

        if (rem >= den) {
            rem -= den;
            quot |= (1U << bit);
        }
    }

    if (rem_out) {
        *rem_out = rem;
    }

    return quot;
}

WEAK_ATTR u32 __mulsi3(u32 lhs, u32 rhs) {
    return _umul32(lhs, rhs);
}

WEAK_ATTR u64 __muldi3(u64 lhs, u64 rhs) {
    return _umul64(lhs, rhs);
}

#if defined(__SIZEOF_INT128__)
WEAK_ATTR __int128 __multi3(__int128 lhs, __int128 rhs) {
    union {
        unsigned __int128 value;
        struct {
            u64 lo;
            u64 hi;
        } parts;
    } a = { .value = (unsigned __int128)lhs }, b = { .value = (unsigned __int128)rhs }, result = { .value = 0 };

    u64 lo_hi = 0;
    u64 lo_lo = 0;
    _umul128_64(a.parts.lo, b.parts.lo, &lo_hi, &lo_lo);

    result.parts.lo = lo_lo;
    result.parts.hi = lo_hi + _umul64(a.parts.lo, b.parts.hi) + _umul64(a.parts.hi, b.parts.lo);

    return (__int128)result.value;
}
#endif

WEAK_ATTR u32 __udivsi3(u32 num, u32 den) {
    return _udivmod32(num, den, 0);
}

WEAK_ATTR u32 __umodsi3(u32 num, u32 den) {
    u32 rem = 0;
    _udivmod32(num, den, &rem);
    return rem;
}

WEAK_ATTR i32 __divsi3(i32 num, i32 den) {
    int neg = 0;
    u32 unum = (u32)num;
    u32 uden = (u32)den;

    if (num < 0) {
        neg ^= 1;
        unum = 0U - unum;
    }

    if (den < 0) {
        neg ^= 1;
        uden = 0U - uden;
    }

    u32 quot = _udivmod32(unum, uden, 0);
    return neg ? (i32)(0U - quot) : (i32)quot;
}

WEAK_ATTR i32 __modsi3(i32 num, i32 den) {
    u32 rem = 0;
    u32 unum = (u32)num;
    u32 uden = (u32)den;

    if (num < 0) {
        unum = 0U - unum;
    }

    if (den < 0) {
        uden = 0U - uden;
    }

    _udivmod32(unum, uden, &rem);
    return num < 0 ? (i32)(0U - rem) : (i32)rem;
}

WEAK_ATTR u64 __ashldi3(u64 value, u32 shift) {
    if (shift >= 64U) {
        return 0;
    }

    while (shift--) {
        value <<= 1;
    }

    return value;
}

WEAK_ATTR u64 __lshrdi3(u64 value, u32 shift) {
    if (shift >= 64U) {
        return 0;
    }

    while (shift--) {
        value >>= 1;
    }

    return value;
}

WEAK_ATTR i64 __ashrdi3(i64 value, u32 shift) {
    if (shift >= 64U) {
        return value < 0 ? -1 : 0;
    }

    while (shift--) {
        value >>= 1;
    }

    return value;
}

WEAK_ATTR i32 __ctzdi2(u64 value) {
    if (!value) {
        return 64;
    }

    i32 count = 0;
    while (!(value & 1ULL)) {
        value >>= 1;
        count++;
    }

    return count;
}

WEAK_ATTR i32 __clzdi2(u64 value) {
    if (!value) {
        return 64;
    }

    i32 count = 0;
    u64 bit = 1ULL << 63;
    while (!(value & bit)) {
        bit >>= 1;
        count++;
    }

    return count;
}

WEAK_ATTR i32 __ucmpdi2(u64 lhs, u64 rhs) {
    if (lhs < rhs) {
        return 0;
    }

    if (lhs > rhs) {
        return 2;
    }

    return 1;
}

WEAK_ATTR i32 __cmpdi2(i64 lhs, i64 rhs) {
    if (lhs < rhs) {
        return 0;
    }

    if (lhs > rhs) {
        return 2;
    }

    return 1;
}
