#pragma once

#include <base/types.h>
#include <stdbool.h>

bool wm_cursor_load(const char* path);
void wm_cursor_unload(void);
bool wm_cursor_draw(u32* frame, u32 fb_width, u32 fb_height, i32 x, i32 y);
