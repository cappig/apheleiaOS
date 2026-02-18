#include "draw.h"

#include <string.h>

static i32 min3(i32 a, i32 b, i32 c) {
    i32 min = a;
    if (b < min) {
        min = b;
    }
    if (c < min) {
        min = c;
    }
    return min;
}

static i32 max3(i32 a, i32 b, i32 c) {
    i32 max = a;
    if (b > max) {
        max = b;
    }
    if (c > max) {
        max = c;
    }
    return max;
}

static i64 edge(draw_point_t a, draw_point_t b, i32 px, i32 py) {
    return (i64)(px - a.x) * (i64)(b.y - a.y) - (i64)(py - a.y) * (i64)(b.x - a.x);
}

void draw_rect(
    u32 *fb,
    u32 fb_width,
    u32 fb_height,
    i32 x,
    i32 y,
    u32 width,
    u32 height,
    u32 color
) {
    if (!fb || !width || !height) {
        return;
    }

    i32 x0 = x;
    i32 y0 = y;
    i32 x1 = x + (i32)width;
    i32 y1 = y + (i32)height;

    if (x0 < 0) {
        x0 = 0;
    }
    if (y0 < 0) {
        y0 = 0;
    }
    if (x1 > (i32)fb_width) {
        x1 = (i32)fb_width;
    }
    if (y1 > (i32)fb_height) {
        y1 = (i32)fb_height;
    }

    if (x0 >= x1 || y0 >= y1) {
        return;
    }

    size_t span = (size_t)(x1 - x0);
    size_t span_bytes = span * sizeof(u32);

    // Check if all 4 bytes are the same — memset is fastest for this case
    u8 b0 = (u8)color;
    bool byte_fill =
        (b0 == (u8)(color >> 8)) && (b0 == (u8)(color >> 16)) && (b0 == (u8)(color >> 24));

    if (byte_fill) {
        for (i32 row = y0; row < y1; row++) {
            memset(fb + (size_t)row * fb_width + (size_t)x0, (int)b0, span_bytes);
        }
        return;
    }

    // Fill the first row manually, then memcpy to subsequent rows
    u32 *first_row = fb + (size_t)y0 * fb_width + (size_t)x0;

    for (size_t i = 0; i < span; i++) {
        first_row[i] = color;
    }

    for (i32 row = y0 + 1; row < y1; row++) {
        memcpy(fb + (size_t)row * fb_width + (size_t)x0, first_row, span_bytes);
    }
}

void draw_triangle(
    u32 *fb,
    u32 fb_width,
    u32 fb_height,
    draw_point_t p0,
    draw_point_t p1,
    draw_point_t p2,
    u32 color
) {
    if (!fb || !fb_width || !fb_height) {
        return;
    }

    i32 min_x = min3(p0.x, p1.x, p2.x);
    i32 max_x = max3(p0.x, p1.x, p2.x);
    i32 min_y = min3(p0.y, p1.y, p2.y);
    i32 max_y = max3(p0.y, p1.y, p2.y);

    if (min_x < 0) {
        min_x = 0;
    }
    if (min_y < 0) {
        min_y = 0;
    }
    if (max_x >= (i32)fb_width) {
        max_x = (i32)fb_width - 1;
    }
    if (max_y >= (i32)fb_height) {
        max_y = (i32)fb_height - 1;
    }

    for (i32 y = min_y; y <= max_y; y++) {
        for (i32 x = min_x; x <= max_x; x++) {
            i64 w0 = edge(p0, p1, x, y);
            i64 w1 = edge(p1, p2, x, y);
            i64 w2 = edge(p2, p0, x, y);

            if (!((w0 >= 0 && w1 >= 0 && w2 >= 0) || (w0 <= 0 && w1 <= 0 && w2 <= 0))) {
                continue;
            }

            fb[(size_t)y * fb_width + (size_t)x] = color;
        }
    }
}

void draw_polygon(
    u32 *fb,
    u32 fb_width,
    u32 fb_height,
    const draw_point_t *points,
    size_t count,
    u32 color
) {
    if (!fb || !points || count < 3) {
        return;
    }

    draw_point_t first = points[0];
    for (size_t i = 1; i + 1 < count; i++) {
        draw_triangle(fb, fb_width, fb_height, first, points[i], points[i + 1], color);
    }
}
