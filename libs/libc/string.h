#pragma once

#include "stddef.h"

// #include_next <string.h> //TODO: why the fuck is this broken [clangd?]
#ifdef EXTEND_LIBC
#include <libc_ext/string.h>
#endif


void* memcpy(void* restrict dest, const void* restrict src, size_t len);
void* memmove(void* dest, const void* src, size_t len);
char* strcpy(char* restrict dest, const char* restrict src);
char* strncpy(char* restrict dest, const char* restrict src, size_t len);

char* strcat(char* dest, const char* src);
char* strncat(char* dest, const char* src, size_t len);

int memcmp(const void* s1, const void* s2, size_t n);
int strcmp(const char* s1, const char* s2);
int strncmp(const char* s1, const char* s2, size_t n);
// TODO: missing the locale stuff

char* strchr(const char* str, int ch);
char* strrchr(const char* str, int ch);
size_t strcspn(const char* dest, const char* src);
// TODO: Some stuff missing here too

void* memset(void* dest, int val, size_t len);
int strlen(const char* str);

void* memchr(const void* ptr, int ch, size_t len);

char* strtok_r(char* str, const char* delim, char** saveptr);
char* strtok(char* restrict str, const char* restrict delim);

char* strdup(const char* src);
char* strndup(const char* str, size_t size);
