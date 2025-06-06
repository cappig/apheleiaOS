#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>


void memswap(void* a, void* b, size_t len) {
    char* a_byte = a;
    char* b_byte = b;

    for (size_t i = 0; i < len; i++) {
        char temp = a_byte[i];

        a_byte[i] = b_byte[i];
        b_byte[i] = temp;
    }
}

char* strrev(char* str) {
    int len = strlen(str);

    if (!str || len <= 1)
        return str;

    for (int i = 0; i < len / 2; i++) {
        char temp = str[len - 1 - i];
        str[len - 1 - i] = str[i];
        str[i] = temp;
    }

    return str;
}

size_t strnlen(const char* str, size_t max) {
    if (!str)
        return 0;

    const char* found = memchr(str, '\0', max);
    return found ? (size_t)(found - str) : max;
}


int strcasecmp(const char* s1, const char* s2) {
    while (*s1 && (tolower(*s1) == tolower(*s2))) {
        s1++;
        s2++;
    }

    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

int strncasecmp(const char* s1, const char* s2, size_t n) {
    while (n && *s1 && (tolower(*s1) == tolower(*s2))) {
        s1++;
        s2++;
        n--;
    }

    if (!n)
        return 0;
    else
        return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}


char* strtrim(char* str) {
    while (isspace(*str))
        str++;

    return str;
}

char* strtrunc(char* str) {
    char* end = str + strlen(str);

    while (end > str && isspace(*end))
        end--;

    end[1] = '\0';

    return str;
}


char* basename_ptr(const char* path) {
    char* slash = strrchr(path, '/');

    if (slash)
        return slash + 1;

    return NULL;
}


char* dirname(char* path) {
    size_t len = strlen(path);

    if (!len)
        return ".";

    size_t i = len - 1;

    // Strip trailing slashes at the end of the path
    while (path[i] == '/') {
        if (!i--)
            return "/";
    }

    // Remove the base name
    while (path[i] != '/') {
        if (!i--)
            return ".";
    }

    // Strip the slashes before the basename
    while (path[i] == '/') {
        if (!i--)
            return "/";
    }

    path[i + 1] = 0;

    return path;
}

char* basename(char* path) {
    size_t len = strlen(path);

    if (!len)
        return ".";

    size_t i = len - 1;

    // Strip trailing slashes at the end of the path
    while (path[i] == '/') {
        path[i] = 0;

        if (!i--)
            return "/";
    }

    // Find the beginning of the basename
    while (path[i] != '/') {
        if (!i--)
            break;
    }

    return path + i + 1;
}
