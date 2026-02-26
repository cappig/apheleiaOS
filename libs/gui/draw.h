#pragma once

#include <gui/fb.h>
#include <stddef.h>

typedef struct {
    i32 x;
    i32 y;
} draw_point_t;

// Colors are encoded as 0x00RRGGBB
enum draw_color {
    DRAW_TRANSPARENT = 0x00000000U,
    DRAW_BLACK = 0x00000000U,
    DRAW_WHITE = 0x00ffffffU,
    DRAW_GRAY_DARK = 0x00303030U,
    DRAW_GRAY = 0x00777777U,
    DRAW_GRAY_LIGHT = 0x00c0c0c0U,
    DRAW_RED = 0x00ff0000U,
    DRAW_GREEN = 0x0000ff00U,
    DRAW_BLUE = 0x000000ffU,
    DRAW_YELLOW = 0x00ffff00U,
    DRAW_CYAN = 0x0000ffffU,
    DRAW_MAGENTA = 0x00ff00ffU,
    DRAW_ORANGE = 0x00ff9900U,
};

void draw_rect(
    framebuffer_t *fb,
    i32 x,
    i32 y,
    u32 width,
    u32 height,
    pixel_t color
);

void draw_line(
    framebuffer_t *fb,
    i32 x0,
    i32 y0,
    i32 x1,
    i32 y1,
    pixel_t color
);

void draw_circle(
    framebuffer_t *fb,
    i32 cx,
    i32 cy,
    u32 radius,
    pixel_t color
);

void draw_disk(
    framebuffer_t *fb,
    i32 cx,
    i32 cy,
    u32 radius,
    pixel_t color
);

void draw_text(
    framebuffer_t *fb,
    i32 x,
    i32 y,
    const char *text,
    pixel_t color
);

bool draw_set_font_path(const char *path);
const char *draw_get_font_path(void);

u32 draw_font_width(void);
u32 draw_font_height(void);

void draw_triangle(
    framebuffer_t *fb,
    draw_point_t p0,
    draw_point_t p1,
    draw_point_t p2,
    pixel_t color
);

void draw_polygon(
    framebuffer_t *fb,
    const draw_point_t *points,
    size_t count,
    pixel_t color
);
