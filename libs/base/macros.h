#pragma once

#define DIV_ROUND_UP(a, b) (((a) + (b) - 1) / (b))

// These only work for powers of 2.
#define __ALIGN_MASK(x, mask) (((x) + (mask)) & ~(mask))

#define ALIGN(x, a)      __ALIGN_MASK(x, (typeof(x))(a) - 1)
#define ALIGN_DOWN(x, a) ALIGN((x) - ((a) - 1), (a))

#define ARRAY_LEN(array) (sizeof(array) / sizeof((array)[0]))

#define EXPECT(x, y) __builtin_expect((x), (y))
#define UNLIKELY(x)  EXPECT((x), false)
#define LIKELY(x)    EXPECT((x), true)
