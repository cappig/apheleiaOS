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

#ifdef EXTERNAL_ALLOC
typedef void *(*libc_malloc_fn_t)(size_t size);
typedef void (*libc_free_fn_t)(void *ptr);

typedef struct {
    libc_malloc_fn_t malloc_fn;
    libc_free_fn_t free_fn;
} libc_alloc_ops_t;

void __libc_init_alloc(const libc_alloc_ops_t *ops);
#endif

void *malloc(size_t size);
void *calloc(size_t num, size_t size);
void *realloc(void *ptr, size_t size);
void free(void *ptr);
void abort(void) __attribute__((noreturn));
void _Exit(int status) __attribute__((noreturn));
void exit(int status) __attribute__((noreturn));
int atexit(void (*fn)(void));

long long strtoll(char const *restrict str, char **restrict endptr, int base);
long strtol(char const *restrict str, char **restrict endptr, int base);
unsigned long long strtoull(
    char const *restrict str,
    char **restrict endptr,
    int base
);
unsigned long strtoul(char const *restrict str, char **restrict endptr, int base);
double strtod(char const *restrict str, char **restrict endptr);
float strtof(char const *restrict str, char **restrict endptr);
long double strtold(char const *restrict str, char **restrict endptr);

double atof(char const *str);
long long atoll(char const *str);
long atol(char const *str);
int atoi(char const *str);

long long llabs(long long n);
long labs(long n);
int abs(int n);
div_t div(int numer, int denom);
ldiv_t ldiv(long numer, long denom);
lldiv_t lldiv(long long numer, long long denom);

char *getenv(const char *name);
int setenv(const char *name, const char *value, int overwrite);
int unsetenv(const char *name);
char *realpath(const char *path, char *resolved_path);

void *bsearch(
    const void *key,
    const void *base,
    size_t nmemb,
    size_t size,
    int (*compar)(const void *, const void *)
);

void qsort(
    void *base,
    size_t num,
    size_t size,
    int (*comp)(const void *, const void *)
);
