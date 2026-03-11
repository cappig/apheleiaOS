#pragma once

#include <base/macros.h>
#include <base/types.h>

typedef struct font_map {
    u32 codepoint;
    u32 glyph;
} font_map_t;

typedef struct font {
    const u8 *glyphs;
    u32 glyph_width;
    u32 glyph_height;
    u32 glyph_count;
    u32 first_char;
    const font_map_t *map;
    u32 map_count;
} font_t;

static inline u32 font_row_bytes(const font_t *font) {
    if (!font) {
        return 0;
    }

    return (u32)DIV_ROUND_UP(font->glyph_width, 8);
}

static inline u32 font_glyph_bytes(const font_t *font) {
    if (!font) {
        return 0;
    }

    return font_row_bytes(font) * font->glyph_height;
}
