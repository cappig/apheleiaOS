#pragma once

#define SIZE_MAX __SIZE_MAX__

#define CHAR_BIT __CHAR_BIT__

#define SCHAR_MAX __SCHAR_MAX__
#define SCHAR_MIN (-SCHAR_MAX - 1)
#define UCHAR_MAX (SCHAR_MAX * 2ULL + 1ULL)

#ifdef __CHAR_UNSIGNED__
#define CHAR_MIN 0
#define CHAR_MAX UCHAR_MAX
#else
#define CHAR_MIN SCHAR_MIN
#define CHAR_MAX SCHAR_MAX
#endif

#define SHRT_MAX  __SHRT_MAX__
#define SHRT_MIN  (-SHRT_MAX - 1)
#define USHRT_MAX (SHRT_MAX * 2ULL + 1ULL)

#define INT_MAX  __INT_MAX__
#define INT_MIN  (-INT_MAX - 1)
#define UINT_MAX (INT_MAX * 2ULL + 1ULL)

#define LONG_MAX  __LONG_MAX__
#define LONG_MIN  (-LONG_MAX - 1)
#define ULONG_MAX (LONG_MAX * 2ULL + 1ULL)

#define LLONG_MAX  __LONG_LONG_MAX__
#define LLONG_MIN  (-LLONG_MAX - 1)
#define ULLONG_MAX (LLONG_MAX * 2ULL + 1ULL)
