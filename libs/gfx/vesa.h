#pragma once

#include <base/types.h>
#include <term/term.h>

#include "state.h"


void vesa_draw_pixel(graphics_state* graphics, usize x, usize y, rgba_color color);
