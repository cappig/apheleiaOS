#pragma once

#include "stddef.h"


void memswap(void* a, void* b, size_t len);

char* strrev(char* str);

size_t strnlen(const char* str, size_t max);

int strcasecmp(const char* s1, const char* s2);
int strncasecmp(const char* s1, const char* s2, size_t n);

char* strtrim(char* str);
char* strtrunc(char* str);

char* basename_ptr(const char* path);

char* dirname(char* path);
char* basename(char* path);
