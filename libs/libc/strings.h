#pragma once

#include "stddef.h"
#include "string.h"

int bcmp(const void *s1, const void *s2, size_t n);
void bcopy(const void *src, void *dest, size_t len);
void bzero(void *dest, size_t len);

char *index(const char *str, int ch);
char *rindex(const char *str, int ch);

