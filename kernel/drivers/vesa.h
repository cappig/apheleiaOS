#pragma once

#include <base/types.h>
#include <gfx/color.h>
#include <gfx/state.h>

#include "vfs/fs.h"


u32 to_vesa_color(graphics_state* gfx, rgba_color color);

void vesa_draw_pixel(graphics_state* gfx, usize x, usize y, u32 color);

void init_framebuffer(virtual_fs* vfs, graphics_state* gfx);
