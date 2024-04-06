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

    char* token = str;

    while (*str && !_is_delim(*str, delim))
        str++;

    if (*str)
        *str++ = '\0';

    *save_ptr = str;

    return token;
}
