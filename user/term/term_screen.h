#pragma once

#include <base/types.h>
#include <gui/fb.h>
#include <stdbool.h>
#include <stddef.h>

#define TERM_MAX_COLS 160
#define TERM_MAX_ROWS 64

bool term_screen_init(const framebuffer_t *fb);
bool term_screen_resize(const framebuffer_t *fb);
bool term_screen_can_resize(u32 width, u32 height);
void term_screen_reset(void);
void term_screen_feed(const u8 *bytes, size_t len);
bool term_screen_render_rect(u32 *x, u32 *y, u32 *width, u32 *height);
size_t term_screen_cols(void);
size_t term_screen_rows(void);
