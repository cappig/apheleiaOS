#pragma once

#include <base/attributes.h>
#include <base/types.h>

// There are 63 printable characters (48 witouth the numpad)
// This means that a keymap must provide two sets of 64 characters for a total of 128 per keymap
typedef const u8 ascii_keymap[2][64];
typedef const u16 unicode_keymap[2][64];


// Default EN-US ASCII keymap
static ascii_keymap us_keymap = {
    {
        0,   'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i',  'j', 'k',  'l', 'm', 'n', 'o',
        'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y',  'z', '0',  '1', '2', '3', '4',
        '5', '6', '7', '8', '9', '-', '=', '[', ']', '\\', ';', '\'', '`', ',', '.', '/',
        ' ', '/', '*', '-', '+', '.', '0', '1', '2', '3',  '4', '5',  '6', '7', '8', '9',
    },
    {
        0,   'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K',  'L', 'M', 'N', 'O',
        'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', ')',  '!', '@', '#', '$',
        '%', '^', '&', '*', '(', '_', '+', '{', '}', '|', ':', '\"', '~', '<', '>', '?',
        ' ', '/', '*', '-', '+', '.', '0', '1', '2', '3', '4', '5',  '6', '7', '8', '9',
    },
};
