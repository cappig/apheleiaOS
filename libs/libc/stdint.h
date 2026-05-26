#pragma once

#ifndef __INT8_TYPE__
#define __INT8_TYPE__ signed char
#endif

#ifndef __INT16_TYPE__
#define __INT16_TYPE__ short
#endif

#ifndef __INT32_TYPE__
#define __INT32_TYPE__ int
#endif

#ifndef __INT64_TYPE__
#define __INT64_TYPE__ long long
#endif

#ifndef __UINT8_TYPE__
#define __UINT8_TYPE__ unsigned char
#endif

#ifndef __UINT16_TYPE__
#define __UINT16_TYPE__ unsigned short
#endif

#ifndef __UINT32_TYPE__
#define __UINT32_TYPE__ unsigned int
#endif

#ifndef __UINT64_TYPE__
#define __UINT64_TYPE__ unsigned long long
#endif

#ifndef __INTMAX_TYPE__
#define __INTMAX_TYPE__ long long
#endif

#ifndef __UINTMAX_TYPE__
#define __UINTMAX_TYPE__ unsigned long long
#endif

#ifndef __INT8_MAX__
#define __INT8_MAX__ 127
#endif

#ifndef __INT16_MAX__
#define __INT16_MAX__ 32767
#endif

#ifndef __INT32_MAX__
#define __INT32_MAX__ 2147483647
#endif

#ifndef __INT64_MAX__
#define __INT64_MAX__ 9223372036854775807LL
#endif

#ifndef __UINT8_MAX__
#define __UINT8_MAX__ 255U
#endif

#ifndef __UINT16_MAX__
#define __UINT16_MAX__ 65535U
#endif

#ifndef __UINT32_MAX__
#define __UINT32_MAX__ 4294967295U
#endif

#ifndef __UINT64_MAX__
#define __UINT64_MAX__ 18446744073709551615ULL
#endif

#ifndef __INTMAX_MAX__
#define __INTMAX_MAX__ __INT64_MAX__
#endif

#ifndef __UINTMAX_MAX__
#define __UINTMAX_MAX__ __UINT64_MAX__
#endif

typedef __INT8_TYPE__ int8_t;
typedef __INT16_TYPE__ int16_t;
typedef __INT32_TYPE__ int32_t;
typedef __INT64_TYPE__ int64_t;

typedef __UINT8_TYPE__ uint8_t;
typedef __UINT16_TYPE__ uint16_t;
typedef __UINT32_TYPE__ uint32_t;
typedef __UINT64_TYPE__ uint64_t;

typedef __INTMAX_TYPE__ intmax_t;
typedef __UINTMAX_TYPE__ uintmax_t;

typedef __UINTPTR_TYPE__ uintptr_t;
typedef __INTPTR_TYPE__ intptr_t;

#define INT8_MAX  __INT8_MAX__
#define INT8_MIN  (-INT8_MAX - 1)
#define INT16_MAX __INT16_MAX__
#define INT16_MIN (-INT16_MAX - 1)
#define INT32_MAX __INT32_MAX__
#define INT32_MIN (-INT32_MAX - 1)
#define INT64_MAX __INT64_MAX__
#define INT64_MIN (-INT64_MAX - 1)

#define UINT8_MAX  __UINT8_MAX__
#define UINT16_MAX __UINT16_MAX__
#define UINT32_MAX __UINT32_MAX__
#define UINT64_MAX __UINT64_MAX__

#define UINTMAX_MAX __UINTMAX_MAX__
#define INTMAX_MAX  __INTMAX_MAX__
#define INTMAX_MIN  (-INTMAX_MAX - 1)

#define UINTPTR_MAX __UINTPTR_MAX__
#define INTPTR_MAX  __INTPTR_MAX__
#define INTPTR_MIN  (-INTPTR_MAX - 1)
