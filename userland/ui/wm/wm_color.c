#include "wm_color.h"

static int _hex_nibble(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }

    if (ch >= 'a' && ch <= 'f') {
        return 10 + (ch - 'a');
    }

    if (ch >= 'A' && ch <= 'F') {
        return 10 + (ch - 'A');
    }

    return -1;
}

bool wm_parse_hex_color(const char *text, u32 *color_out) {
    if (!text || !color_out) {
        return false;
    }

    const char *pos = text;
    if (pos[0] == '#') {
        pos++;
    } else if (pos[0] == '0' && (pos[1] == 'x' || pos[1] == 'X')) {
        pos += 2;
    }

    u32 value = 0;
    size_t digits = 0;
    while (digits < 8) {
        int nib = _hex_nibble(pos[digits]);
        if (nib < 0) {
            break;
        }

        value = (value << 4) | (u32)nib;
        digits++;
    }

    if (pos[digits] != '\0') {
        return false;
    }

    if (digits == 6) {
        *color_out = value;
        return true;
    }

    if (digits == 8) {
        *color_out = value & 0x00ffffffU;
        return true;
    }

    return false;
}
