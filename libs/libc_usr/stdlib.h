#pragma once

#include <stddef.h>


void* malloc(size_t size);
void* calloc(size_t num, size_t size);

void* realloc(void* ptr, size_t size);

void free(void* ptr);

[[noreturn]] void abort(void);

int atexit(void (*func)(void));

char* getenv(const char* name);
