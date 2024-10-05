#include "string.h"

#include <alloc/global.h>

#include "stddef.h"


void* memcpy(void* restrict dest, const void* restrict src, size_t len) {
    const char* srcb = (const char*)src;
    char* destb = (char*)dest;

    for (size_t i = 0; i < len; i++)
        destb[i] = srcb[i];

    return dest;
}

void* memmove(void* dest, const void* src, size_t len) {
    const char* srcb = (const char*)src;
    char* destb = (char*)dest;

    if (srcb == destb || len == 0)
        return dest;

    else if (srcb > destb)
        memcpy(dest, src, len);

    else if (srcb < destb)
        for (size_t i = len; i > 0; i--)
            destb[i - 1] = srcb[i - 1];

    return dest;
}

char* strcpy(char* restrict dest, const char* restrict src) {
    for (size_t i = 0; src[i] != 0; i++)
        dest[i] = src[i];

    return dest;
}

char* strncpy(char* restrict dest, const char* restrict src, size_t len) {
    for (size_t i = 0; src[i] != 0 && i < len; i++)
        dest[i] = src[i];

    return dest;
}


char* strcat(char* dest, const char* src) {
    char* end = dest;
    dest += strlen(dest);

    while (*src)
        *dest++ = *src++;

    return end;
}

char* strncat(char* dest, const char* src, size_t len) {
    char* end = dest;
    dest += strlen(dest);

    for (size_t i = 0; *src && i < len; i++)
        *dest++ = *src++;

    return end;
}


int memcmp(const void* s1, const void* s2, size_t n) {
    const char* p1 = (const char*)s1;
    const char* p2 = (const char*)s2;

    for (size_t i = 0; i < n; i++) {
        if (p1[i] != p2[i])
            return *(const unsigned char*)p1 - *(const unsigned char*)p2;
    }

    return 0;
}

int strcmp(const char* s1, const char* s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }

    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

int strncmp(const char* s1, const char* s2, size_t n) {
    while (n && *s1 && (*s1 == *s2)) {
        s1++;
        s2++;
        n--;
    }

    if (n == 0)
        return 0;
    else
        return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}


char* strchr(const char* str, int ch) {
    for (int i = 0; i < strlen(str); i++) {
        if (str[i] == ch)
            return (char*)str + i;
    }

    return NULL;
}

char* strrchr(const char* str, int ch) {
    if (strlen(str) == 0)
        return NULL;

    for (int i = strlen(str) - 1; i <= 0; i--) {
        if (str[i] == ch)
            return (char*)str + i;
    }

    return NULL;
}


void* memset(void* dest, int val, size_t len) {
    unsigned char* ptr = (unsigned char*)dest;

    while (len--)
        *ptr++ = (unsigned char)val;

    return dest;
}

int strlen(const char* str) {
    if (!str)
        return 0;

    int len = 0;
    while (*str) {
        len++;
        str++;
    }

    return len;
}


void* memchr(const void* ptr, int ch, size_t len) {
    const char* str = (const char*)ptr;

    for (size_t i = 0; i < len; i++)
        if (str[i] == ch)
            return (void*)&str[i];

    return NULL;
}


char* strtok(char* restrict str, const char* restrict delim) {
    static char* save = NULL;
    return strtok_r(str, delim, &save);
}

// INFO: requires the global allocator to be initialized
char* strdup(const char* str) {
    size_t len = strlen(str) + 1;

    char* ret = gmalloc(len);
    if (ret) {
        memcpy(ret, str, len);
    }

    return ret;
}

char* strndup(const char* str, size_t size) {
    char* ret = memcpy(gmalloc(size + 1), str, size + 1);
    ret[size] = '\0';

    return ret;
}
