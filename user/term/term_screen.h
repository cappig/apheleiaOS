#pragma once

#include <base/types.h>
#include <stdbool.h>
#include <stddef.h>

#define TERM_MAX_W      1024
#define TERM_MAX_H      768
#define TERM_MAX_PIXELS (TERM_MAX_W * TERM_MAX_H)

#define TERM_MAX_COLS 160
#define TERM_MAX_ROWS 64

bool term_screen_init(u32 width, u32 height, u32* pixels, size_t pixels_count);
void term_screen_reset(void);
void term_screen_feed(const u8* bytes, size_t len);
void term_screen_render(void);
size_t term_screen_cols(void);
size_t term_screen_rows(void);
