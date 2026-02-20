#pragma once

#include <gui/fb.h>
#include <stddef.h>

typedef struct {
    i32 x;
    i32 y;
} draw_point_t;

void draw_rect(
    framebuffer_t *fb,
    i32 x,
    i32 y,
    u32 width,
    u32 height,
    pixel_t color
);

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
