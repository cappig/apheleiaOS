#pragma once

#include <base/types.h>
#include <gfx/color.h>
#include <gfx/state.h>

#include "vfs/fs.h"


u32 to_vesa_color(graphics_state* gfx, rgba_color color);

bool draw_pixel_rgba(usize x, usize y, rgba_color color);
bool draw_pixel(usize x, usize y, u32 color);

void init_framebuffer(graphics_state* gfx);
void init_framebuffer_dev(void);