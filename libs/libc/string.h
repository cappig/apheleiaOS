#pragma once

#include "stddef.h"

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
// TODO: Some stuff missing here too

void* memset(void* dest, int val, size_t len);
int strlen(const char* str);

void* memchr(const void* ptr, int ch, size_t len);

char* strtok(char* restrict str, const char* restrict delim);

// char* strdup(const char* src);
// char* strndup(const char* str, size_t size);

// #include_next <string.h> //TODO: why the fuck is this broken [clangd?]
#include <libc_ext/string.h>
