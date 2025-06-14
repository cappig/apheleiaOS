#include "string.h"

#ifdef _KERNEL
#include <alloc/global.h>
#define malloc(size) gmalloc(size)
#else
#include <stdlib.h>
#endif

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
    while (*str) {
        if (*str == (char)ch)
            return (char*)str;

        str++;
    }

    return NULL;
}

char* strrchr(const char* str, int ch) {
    if (!str)
        return NULL;

    const char* ptr = NULL;

    while (*str) {
        if (*str == (char)ch)
            ptr = str;

        str++;
    }

    return (char*)ptr;
}

size_t strcspn(const char* dest, const char* src) {
    const char* d = dest;

    while (*d) {
        const char* s = src;

        while (*s) {
            if (*d == *s)
                return (size_t)(d - dest);

            s++;
        }

        d++;
    }

    return (size_t)(d - dest);
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


static bool _is_delim(char c, const char* delim) {
    while (*delim) {
        if (c == *delim)
            return true;

        delim++;
    }

    return false;
}

char* strtok_r(char* str, const char* delim, char** save_ptr) {
    if (!str)
        str = *save_ptr;

    if (!str)
        return NULL;

    if (*str == '\0') {
        *save_ptr = NULL;
        return NULL;
    }

    // Trim leading deliminators
    while (*str && _is_delim(*str, delim))
        str++;

    if (*str == '\0') {
        *save_ptr = str;
        return NULL;
    }

    // Find the end of the token
    char* end = str;
    while (*end && !_is_delim(*end, delim))
        end++;

    if (*end == '\0') {
        *save_ptr = end;
        return str;
    }

    *end = '\0';
    *save_ptr = end + 1;

    return str;
}

char* strtok(char* restrict str, const char* restrict delim) {
    static char* save = NULL;
    return strtok_r(str, delim, &save);
}


char* strndup(const char* str, size_t size) {
    if (!size || !str)
        return NULL;

    char* dest = malloc(size + 1);

    if (dest) {
        strncpy(dest, str, size);
        dest[size] = '\0';
    }

    return dest;
}

// NOTE:  This function triggers a false positive in gcc's static analyser
char* strdup(const char* str) {
    return strndup(str, strlen(str));
}
