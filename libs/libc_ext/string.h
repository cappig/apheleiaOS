#pragma once

#include "stddef.h"

void memswap(void* a, void* b, size_t len);

char* strrev(char* str);

size_t strnlen(const char* str, size_t max);

char* strtok_r(char* str, const char* delim, char** saveptr);
