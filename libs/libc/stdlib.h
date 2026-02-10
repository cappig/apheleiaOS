#pragma once

#include "stddef.h"

#define EXIT_FAILURE 1
#define EXIT_SUCCESS 0

typedef struct {
    int quot, rem;
} div_t;

typedef struct {
    long quot, rem;
} ldiv_t;

typedef struct {
    long long quot, rem;
} lldiv_t;


#ifndef NO_LIBC_EXTENTIONS
#include <libc_ext/stdlib.h>
#endif

#ifndef _KERNEL
#include <libc_usr/stdlib.h>
#endif

// We can hook external allocators here. We use this in the kernel/bootloader
#ifdef EXTERNAL_ALLOC
typedef void* (*malloc_fn)(size_t size);
typedef void* (*realloc_fn)(void* ptr, size_t size);
typedef void (*free_fn)(void* ptr);

// This structure is kept in global memory
struct _external_alloc {
    malloc_fn malloc;
    // realloc_fn realloc;

    free_fn free;
};

void* malloc(size_t size);
void* calloc(size_t num, size_t size);

void free(void* ptr);
#endif

long long strtoll(char const* restrict str, char** restrict endptr, int base);
long strtol(char const* restrict str, char** restrict endptr, int base);
// unsigned long int strtoul(char const* restrict str, char** restrict endptr, int base);
// double strtod(char const* restrict str, char** restrict endptr);

// double atof(char const* str);
long long atoll(char const* str);
long atol(char const* str);
int atoi(char const* str);

long long llabs(long long n);
long labs(long n);
int abs(int n);

void qsort(void* base, size_t num, size_t size, int (*comp)(const void*, const void*));
