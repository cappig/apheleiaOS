#include "ctype.h"


int isalnum(int c) {
    if (isdigit(c) || isalpha(c))
        return 1;

    return 0;
}

int isalpha(int c) {
    if (c >= 'a' && c <= 'z')
        return 1;

    if (c >= 'A' && c <= 'Z')
        return 1;

    return 0;
}

int isblank(int c) {
    if (c == ' ' || c == '\t')
        return 1;

    return 0;
}

int iscntrl(int c) {
    if (c == 0x7f)
        return 1;

    if (c >= 0x00 && c <= 0x1f)
        return 1;

    return 0;
}

int isdigit(int c) {
    if (c >= '0' && c <= '9')
        return 1;

    return 0;
}

int isgraph(int c) {
    if (c >= 0x21 && c <= 0x7f)
        return 1;

    return 0;
}

int islower(int c) {
    if (c >= 'a' && c <= 'z')
        return 1;

    return 0;
}

int isprint(int c) {
    if (c >= 0x20 && c <= 0x7e)
        return 1;

    return 0;
}

int ispunct(int c) {
    if (c < 0x1f || c == 0x7f)
        return 1;

    return 0;
}

int isspace(int c) {
    if (c == ' ')
        return 1;

    if (c >= 0x9 && c <= 0xd)
        return 1;

    return 0;
}

int isupper(int c) {
    if (c >= 'A' && c <= 'Z')
        return 1;

    return 0;
}

int isxdigit(int c) {
    if (isdigit(c))
        return 1;

    if (c >= 'a' && c <= 'f')
        return 1;

    if (c >= 'A' && c <= 'F')
        return 1;

    return 0;
}

int tolower(int c) {
    if (isupper(c))
        return c - ('A' - 'a');

    return c;
}

int toupper(int c) {
    if (islower(c))
        return c - ('a' - 'A');

    return c;
}
