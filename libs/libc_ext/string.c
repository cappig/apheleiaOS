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


char* basename(const char* path) {
    char* slash = strrchr(path, '/');

    if (slash)
        return slash + 1;
    else
        return (char*)path;
}

#ifdef HAS_GMALLOC
char* dirname(const char* path) {
    char* slash = strrchr(path, '/');

    if (!slash)
        return NULL;

    return strndup(path, slash - path);
}
#endif
