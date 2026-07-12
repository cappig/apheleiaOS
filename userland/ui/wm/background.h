#pragma once

#include <stdbool.h>

#include "wm.h"

bool wm_background_load(u32 fb_width, u32 fb_height, const char *path);
void wm_background_unload(void);
bool wm_background_draw(pixel_t *frame, u32 fb_width, u32 fb_height);
bool wm_background_draw_rect(pixel_t *frame, u32 fb_width, u32 fb_height, const wm_rect_t *rect);
