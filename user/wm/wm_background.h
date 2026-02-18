#pragma once

#include <base/types.h>
#include <stdbool.h>

bool wm_background_load(u32 fb_width, u32 fb_height, const char *path);
void wm_background_unload(void);
bool wm_background_draw(u32 *frame, u32 fb_width, u32 fb_height);
