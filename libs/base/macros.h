#pragma once

#define KiB (1024)
#define MiB (KiB * 1024)
#define GiB (MiB * 1024)
#define TiB (GiB * 1024)

#define DIV_ROUND_UP(a, b) (((a) + (b)-1) / (b))

// Stolen from the linux kernel
// These only work for powers of 2 but that's the only case where we use them
#define __ALIGN_MASK(x, mask) (((x) + (mask)) & ~(mask))

#define ALIGN(x, a)      __ALIGN_MASK(x, (typeof(x))(a)-1)
#define ALIGN_DOWN(x, a) ALIGN((x) - ((a)-1), (a))

#define ARRAY_LEN(array) (sizeof(array) / sizeof((array)[0]))
