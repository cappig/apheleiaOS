#pragma once

typedef __SIZE_TYPE__ size_t;
typedef __PTRDIFF_TYPE__ ptrdiff_t;

#if __STDC_VERSION__ < 202311L
    #define NULL ((void*)0)
#else
    #define NULL nullptr
#endif

#define offsetof(type, member) __builtin_offsetof(type, member)
