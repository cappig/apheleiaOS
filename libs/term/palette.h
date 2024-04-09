#pragma once

#include <base/types.h>

#include "term.h"


u16 term_char_to_vga(term_char ch);

void term_set_palette(terminal* term, const u32 palette[16]);
