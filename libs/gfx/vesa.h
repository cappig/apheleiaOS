#pragma once

#include <base/attributes.h>
#include <base/types.h>
#include <term/term.h>

#include "state.h"


void vesa_draw_pixel(graphics_state* graphics, usize x, usize y, u32 color) ATTRIBUTE(nonnull);

void vesa_putc(graphics_state* graphics, u32 x, u32 y, term_char ch) ATTRIBUTE(nonnull);
