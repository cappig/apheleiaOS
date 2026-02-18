#pragma once

#include <base/types.h>
#include <stddef.h>

typedef struct {
    i32 x;
    i32 y;
} draw_point_t;

void draw_rect(
    u32 *fb,
    u32 fb_width,
    u32 fb_height,
    i32 x,
    i32 y,
    u32 width,
    u32 height,
    u32 color
);

void draw_triangle(
    u32 *fb,
    u32 fb_width,
    u32 fb_height,
    draw_point_t p0,
    draw_point_t p1,
    draw_point_t p2,
    u32 color
);

void draw_polygon(
    u32 *fb,
    u32 fb_width,
    u32 fb_height,
    const draw_point_t *points,
    size_t count,
    u32 color
);
