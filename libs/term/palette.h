#pragma once

#include <base/types.h>

#include "term.h"


u16 term_cell_to_vga(term_cell* cell);

void term_set_palette(terminal* term, const u32 palette[static 16]);
