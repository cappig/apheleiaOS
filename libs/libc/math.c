#include "math.h"

#include "errno.h"
#include "limits.h"

#define CONST_PI      3.141592653589793238462643383279502884L
#define CONST_PI_2    1.570796326794896619231321691639751442L
#define CONST_2PI     6.283185307179586476925286766559005768L
#define CONST_LN2     0.693147180559945309417232121458176568L
#define CONST_INV_LN2 1.442695040888963407359924681001892137L
#define CONST_LOG2E   1.442695040888963407359924681001892137L
#define CONST_LOG10_2 0.301029995663981195213738894724493027L

typedef enum {
    ROUND_NEAREST = 0,
    ROUND_DOWN = 1,
    ROUND_UP = 2,
    ROUND_ZERO = 3,
} round_mode_t;

static int __isnanl(long double x) {
    return __builtin_isnan(x);
}

static int __isinfl(long double x) {
    return __builtin_isinf_sign(x) != 0;
}

static int __isfinitel(long double x) {
    return __builtin_isfinite(x);
}

static int __signbitl(long double x) {
    return (x < 0.0L) || ((x == 0.0L) && ((1.0L / x) < 0.0L));
}

static long double __fabsl(long double x) {
    if (x < 0.0L) {
        return -x;
    }
    return x;
}

static long double __trunc_rawl(long double x) {
    if (__isnanl(x) || __isinfl(x)) {
        return x;
    }

    if ((x >= (long double)LONG_MAX) || (x <= (long double)LONG_MIN)) {
        return x;
    }

    long i = (long)x;
    return (long double)i;
}

static long double __roundl_mode(long double x, round_mode_t mode) {
    if (__isnanl(x) || __isinfl(x) || (x == 0.0L)) {
        return x;
    }

    long double t = __trunc_rawl(x);

    switch (mode) {
    case ROUND_DOWN:
        if (t > x) {
            t -= 1.0L;
        }
        break;
    case ROUND_UP:
        if (t < x) {
            t += 1.0L;
        }
        break;
    case ROUND_ZERO:
        break;
    case ROUND_NEAREST:
    default: {
        long double frac = x - t;
        if (frac >= 0.5L) {
            t += 1.0L;
        } else if (frac <= -0.5L) {
            t -= 1.0L;
        }
        break;
    }
    }

    return t;
}

static long double __truncl(long double x) {
    return __roundl_mode(x, ROUND_ZERO);
}

static long double __floorl(long double x) {
    return __roundl_mode(x, ROUND_DOWN);
}

static long double __ceill(long double x) {
    return __roundl_mode(x, ROUND_UP);
}

static long double __scalblnl(long double x, long n) {
    if ((x == 0.0L) || __isnanl(x) || __isinfl(x) || (n == 0)) {
        return x;
    }

    int neg = 0;
    unsigned long mag = 0;

    if (n < 0) {
        neg = 1;
        mag = (unsigned long)(-(n + 1));
        mag += 1u;
    } else {
        mag = (unsigned long)n;
    }

    long double r = x;
    long double factor = 2.0L;

    while (mag != 0u) {
        if ((mag & 1u) != 0u) {
            if (neg) {
                r /= factor;
            } else {
                r *= factor;
            }
        }
        factor *= factor;
        mag >>= 1u;
    }

    return r;
}

static long double __frexpl(long double x, int *exp) {
    if (exp != 0) {
        *exp = 0;
    }

    if ((x == 0.0L) || __isnanl(x) || __isinfl(x)) {
        return x;
    }

    int e = 0;
    long double m = x;
    long double ax = __fabsl(m);

    while (ax >= 1.0L) {
        m *= 0.5L;
        ax *= 0.5L;
        e += 1;
    }

    while (ax < 0.5L) {
        m *= 2.0L;
        ax *= 2.0L;
        e -= 1;
    }

    if (exp != 0) {
        *exp = e;
    }

    return m;
}

static long double __exp2_fraction(long double x) {
    long double y = x * CONST_LN2;
    long double term = 1.0L;
    long double sum = 1.0L;

    for (int i = 1; i <= 24; i++) {
        term *= y / (long double)i;
        sum += term;
    }

    return sum;
}

static long double __reduce_angle(long double x) {
    if (__isnanl(x) || __isinfl(x)) {
        return x;
    }

    long double k = __truncl(x / CONST_2PI);
    x -= k * CONST_2PI;

    while (x > CONST_PI) {
        x -= CONST_2PI;
    }

    while (x < -CONST_PI) {
        x += CONST_2PI;
    }

    return x;
}

static long double __sinl_poly(long double x) {
    long double x2 = x * x;
    long double term = x;
    long double sum = x;

    for (int i = 1; i <= 8; i++) {
        long double a = (long double)(2 * i);
        long double b = (long double)(2 * i + 1);
        term *= -(x2 / (a * b));
        sum += term;
    }

    return sum;
}

static long double __cosl_poly(long double x) {
    long double x2 = x * x;
    long double term = 1.0L;
    long double sum = 1.0L;

    for (int i = 1; i <= 8; i++) {
        long double a = (long double)(2 * i - 1);
        long double b = (long double)(2 * i);
        term *= -(x2 / (a * b));
        sum += term;
    }

    return sum;
}

static long double __sinl_approx(long double x) {
    x = __reduce_angle(x);

    if (x > CONST_PI_2) {
        x = CONST_PI - x;
    } else if (x < -CONST_PI_2) {
        x = -CONST_PI - x;
    }

    return __sinl_poly(x);
}

static long double __cosl_approx(long double x) {
    int sign = 1;

    x = __reduce_angle(x);

    if (x > CONST_PI_2) {
        x = CONST_PI - x;
        sign = -1;
    } else if (x < -CONST_PI_2) {
        x = -CONST_PI - x;
        sign = -1;
    }

    long double r = __cosl_poly(x);
    if (sign < 0) {
        return -r;
    }
    return r;
}

static long double __tanl_approx(long double x) {
    long double s = __sinl_approx(x);
    long double c = __cosl_approx(x);

    if (c == 0.0L) {
        if (__signbitl(s)) {
            return -(long double)HUGE_VAL;
        }
        return (long double)HUGE_VAL;
    }

    return s / c;
}

static long double __atanl_series(long double z) {
    long double z2 = z * z;
    long double term = z;
    long double sum = z;

    for (int i = 1; i <= 18; i++) {
        term *= -z2;
        sum += term / (long double)(2 * i + 1);
    }

    return sum;
}

static long double __atanl_approx(long double z) {
    if (z == 0.0L) {
        return 0.0L;
    }

    int neg = 0;
    if (z < 0.0L) {
        neg = 1;
        z = -z;
    }

    long double r = 0.0L;
    if (z > 1.0L) {
        r = CONST_PI_2 - __atanl_series(1.0L / z);
    } else {
        r = __atanl_series(z);
    }

    if (neg) {
        return -r;
    }
    return r;
}

static long double __atan2l_approx(long double y, long double x) {
    if (x > 0.0L) {
        return __atanl_approx(y / x);
    }

    if (x < 0.0L) {
        if (y >= 0.0L) {
            return __atanl_approx(y / x) + CONST_PI;
        }
        return __atanl_approx(y / x) - CONST_PI;
    }

    if (y > 0.0L) {
        return CONST_PI_2;
    }

    if (y < 0.0L) {
        return -CONST_PI_2;
    }

    return 0.0L;
}

static long double __sqrtl_approx(long double x) {
    if (x <= 0.0L) {
        return x;
    }

    long double r = (x >= 1.0L) ? x : 1.0L;
    for (int i = 0; i < 40; i++) {
        r = 0.5L * (r + (x / r));
    }

    return r;
}

static long double __log2l_positive(long double x) {
    int e = 0;
    long double m = __frexpl(x, &e);

    m *= 2.0L;
    e -= 1;

    long double y = (m - 1.0L) / (m + 1.0L);
    long double y2 = y * y;
    long double term = y;
    long double sum = 0.0L;

    for (int d = 1; d <= 39; d += 2) {
        sum += term / (long double)d;
        term *= y2;
    }

    long double ln_m = 2.0L * sum;
    return (long double)e + (ln_m * CONST_INV_LN2);
}

static long double __expl(long double x) {
    if (__isnanl(x)) {
        return x;
    }

    if (x == 0.0L) {
        return 1.0L;
    }

    if (__isinfl(x)) {
        if (x > 0.0L) {
            return (long double)HUGE_VAL;
        }
        return 0.0L;
    }

    long double scaled = x * CONST_LOG2E;

    if (scaled > (long double)LONG_MAX) {
        return (long double)HUGE_VAL;
    }

    if (scaled < (long double)LONG_MIN) {
        return 0.0L;
    }

    long n = (long)__floorl(scaled);
    long double frac = scaled - (long double)n;
    long double base = __exp2_fraction(frac);

    return __scalblnl(base, n);
}

static long double __logl_positive(long double x) {
    return __log2l_positive(x) * CONST_LN2;
}

static double __domain_error(void) {
    errno = EDOM;
    return __builtin_nan("");
}

static double __range_overflow(void) {
    errno = ERANGE;
    return HUGE_VAL;
}

static double __range_underflow(long double sign_src) {
    errno = ERANGE;
    if (__signbitl(sign_src)) {
        return -0.0;
    }
    return 0.0;
}

double acos(double x) {
    if (__isnanl(x)) {
        return x;
    }

    if ((x < -1.0) || (x > 1.0)) {
        return __domain_error();
    }

    return atan2(sqrt(1.0 - (x * x)), x);
}

double asin(double x) {
    if (__isnanl(x)) {
        return x;
    }

    if ((x < -1.0) || (x > 1.0)) {
        return __domain_error();
    }

    return atan2(x, sqrt(1.0 - (x * x)));
}

double atan(double x) {
    if (__isnanl(x)) {
        return x;
    }

    return atan2(x, 1.0);
}

double atan2(double y, double x) {
    if (__isnanl(y) || __isnanl(x)) {
        return __builtin_nan("");
    }

    return (double)__atan2l_approx((long double)y, (long double)x);
}

double cos(double x) {
    if (__isnanl(x)) {
        return x;
    }

    if (__isinfl(x)) {
        return __domain_error();
    }

    return (double)__cosl_approx((long double)x);
}

double sin(double x) {
    if (__isnanl(x)) {
        return x;
    }

    if (__isinfl(x)) {
        return __domain_error();
    }

    return (double)__sinl_approx((long double)x);
}

double tan(double x) {
    if (__isnanl(x)) {
        return x;
    }

    if (__isinfl(x)) {
        return __domain_error();
    }

    return (double)__tanl_approx((long double)x);
}

double cosh(double x) {
    if (__isnanl(x)) {
        return x;
    }

    if (__isinfl(x)) {
        return HUGE_VAL;
    }

    long double ex = __expl((long double)x);
    long double inv = 1.0L / ex;
    long double r = 0.5L * (ex + inv);

    if (!__isfinitel(r)) {
        errno = ERANGE;
    }

    return (double)r;
}

double sinh(double x) {
    if (__isnanl(x)) {
        return x;
    }

    if (__isinfl(x)) {
        if (x < 0.0) {
            return -HUGE_VAL;
        }
        return HUGE_VAL;
    }

    long double ex = __expl((long double)x);
    long double inv = 1.0L / ex;
    long double r = 0.5L * (ex - inv);

    if (!__isfinitel(r)) {
        errno = ERANGE;
    }

    return (double)r;
}

double tanh(double x) {
    if (__isnanl(x)) {
        return x;
    }

    if (x > 20.0) {
        return 1.0;
    }

    if (x < -20.0) {
        return -1.0;
    }

    long double e2x = __expl(2.0L * (long double)x);
    return (double)((e2x - 1.0L) / (e2x + 1.0L));
}

double exp(double x) {
    long double r = __expl((long double)x);

    if ((__isnanl(r))) {
        return (double)r;
    }

    if (!__isfinitel(r)) {
        return __range_overflow();
    }

    if ((r == 0.0L) && (x != 0.0)) {
        return __range_underflow(x);
    }

    return (double)r;
}

double frexp(double x, int *exp) {
    if (exp != 0) {
        *exp = 0;
    }

    long double lx = (long double)x;

    if ((lx == 0.0L) || __isnanl(lx) || __isinfl(lx)) {
        return x;
    }

    int e = 0;
    long double r = __frexpl(lx, &e);

    if (exp != 0) {
        *exp = e;
    }

    return (double)r;
}

double ldexp(double x, int exp) {
    long double lx = (long double)x;

    if ((lx == 0.0L) || __isnanl(lx) || __isinfl(lx)) {
        return x;
    }

    long double r = __scalblnl(lx, (long)exp);

    if (!__isfinitel(r)) {
        return __range_overflow();
    }

    if (r == 0.0L) {
        return __range_underflow(x);
    }

    return (double)r;
}

double log(double x) {
    long double lx = (long double)x;

    if (__isnanl(lx)) {
        return x;
    }

    if (lx < 0.0L) {
        return __domain_error();
    }

    if (lx == 0.0L) {
        errno = ERANGE;
        return -HUGE_VAL;
    }

    if (__isinfl(lx)) {
        return HUGE_VAL;
    }

    return (double)__logl_positive(lx);
}

double log10(double x) {
    long double lx = (long double)x;

    if (__isnanl(lx)) {
        return x;
    }

    if (lx < 0.0L) {
        return __domain_error();
    }

    if (lx == 0.0L) {
        errno = ERANGE;
        return -HUGE_VAL;
    }

    if (__isinfl(lx)) {
        return HUGE_VAL;
    }

    return (double)(__log2l_positive(lx) * CONST_LOG10_2);
}

double modf(double x, double *iptr) {
    long double lx = (long double)x;

    if (iptr == 0) {
        return x;
    }

    if (__isnanl(lx)) {
        *iptr = x;
        return x;
    }

    if (__isinfl(lx)) {
        *iptr = x;
        if (x < 0.0) {
            return -0.0;
        }
        return 0.0;
    }

    long double i = __truncl(lx);
    *iptr = (double)i;
    return (double)(lx - i);
}

double pow(double x, double y) {
    long double lx = (long double)x;
    long double ly = (long double)y;

    if (__isnanl(lx) || __isnanl(ly)) {
        return __builtin_nan("");
    }

    if (ly == 0.0L) {
        return 1.0;
    }

    if (lx == 1.0L) {
        return 1.0;
    }

    if (lx == 0.0L) {
        if (ly < 0.0L) {
            return __range_overflow();
        }
        return 0.0;
    }

    if (lx < 0.0L) {
        long double iy = __truncl(ly);

        if (iy != ly) {
            return __domain_error();
        }

        long double r = __expl(ly * __logl_positive(-lx));
        long double half = __truncl(iy * 0.5L);
        long double odd = iy - (2.0L * half);

        if (odd != 0.0L) {
            r = -r;
        }

        if (!__isfinitel(r)) {
            return __range_overflow();
        }

        if (r == 0.0L) {
            return __range_underflow(x);
        }

        return (double)r;
    }

    long double r = __expl(ly * __logl_positive(lx));

    if (!__isfinitel(r)) {
        return __range_overflow();
    }

    if (r == 0.0L) {
        return __range_underflow(x);
    }

    return (double)r;
}

double sqrt(double x) {
    if (__isnanl(x)) {
        return x;
    }

    if (x < 0.0) {
        return __domain_error();
    }

    if (__isinfl(x)) {
        return x;
    }

    return (double)__sqrtl_approx((long double)x);
}

double ceil(double x) {
    if (__isnanl(x) || __isinfl(x) || (x == 0.0)) {
        return x;
    }

    return (double)__ceill((long double)x);
}

double fabs(double x) {
    if (x < 0.0) {
        return -x;
    }

    return x;
}

double floor(double x) {
    if (__isnanl(x) || __isinfl(x) || (x == 0.0)) {
        return x;
    }

    return (double)__floorl((long double)x);
}

double fmod(double x, double y) {
    long double lx = (long double)x;
    long double ly = (long double)y;

    if (__isnanl(lx) || __isnanl(ly)) {
        return __builtin_nan("");
    }

    if ((ly == 0.0L) || (__isinfl(lx) && __isfinitel(ly))) {
        return __domain_error();
    }

    if (__isinfl(ly) || (lx == 0.0L)) {
        return x;
    }

    long double q = __truncl(lx / ly);
    long double r = lx - (q * ly);

    if ((r == 0.0L) && __signbitl(lx)) {
        return -0.0;
    }

    return (double)r;
}
