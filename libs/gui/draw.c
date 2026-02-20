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
    return (i64)(px - a.x) * (i64)(b.y - a.y) -
           (i64)(py - a.y) * (i64)(b.x - a.x);
}

static size_t _stride_pixels(const framebuffer_t *fb) {
    if (!fb || !fb->width) {
        return 0;
    }

    if (!fb->stride) {
        return fb->width;
    }

    size_t stride = (size_t)fb->stride / sizeof(pixel_t);
    if (!stride || stride < fb->width) {
        return fb->width;
    }

    return stride;
}

void draw_rect(
    framebuffer_t *fb,
    i32 x,
    i32 y,
    u32 width,
    u32 height,
    pixel_t color
) {
    if (!fb || !fb->pixels || !fb->width || !fb->height || !width || !height) {
        return;
    }

    size_t stride_pixels = _stride_pixels(fb);
    if (!stride_pixels) {
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
    if (x1 > (i32)fb->width) {
        x1 = (i32)fb->width;
    }
    if (y1 > (i32)fb->height) {
        y1 = (i32)fb->height;
    }

    if (x0 >= x1 || y0 >= y1) {
        return;
    }

    size_t span = (size_t)(x1 - x0);
    size_t span_bytes = span * sizeof(pixel_t);

    // Check if all 4 bytes are the same — memset is fastest for this case
    u8 b0 = (u8)color;
    bool byte_fill =
        (b0 == (u8)(color >> 8)) && (b0 == (u8)(color >> 16)) && (b0 == (u8)(color >> 24));

    if (byte_fill) {
        for (i32 row = y0; row < y1; row++) {
            memset(
                fb->pixels + (size_t)row * stride_pixels + (size_t)x0,
                (int)b0,
                span_bytes
            );
        }
        return;
    }

    // Fill the first row manually, then memcpy to subsequent rows
    pixel_t *first_row = fb->pixels + (size_t)y0 * stride_pixels + (size_t)x0;

    for (size_t i = 0; i < span; i++) {
        first_row[i] = color;
    }

    for (i32 row = y0 + 1; row < y1; row++) {
        memcpy(
            fb->pixels + (size_t)row * stride_pixels + (size_t)x0,
            first_row,
            span_bytes
        );
    }
}

void draw_triangle(
    framebuffer_t *fb,
    draw_point_t p0,
    draw_point_t p1,
    draw_point_t p2,
    pixel_t color
) {
    if (!fb || !fb->pixels || !fb->width || !fb->height) {
        return;
    }

    size_t stride_pixels = _stride_pixels(fb);
    if (!stride_pixels) {
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
    if (max_x >= (i32)fb->width) {
        max_x = (i32)fb->width - 1;
    }
    if (max_y >= (i32)fb->height) {
        max_y = (i32)fb->height - 1;
    }

    for (i32 y = min_y; y <= max_y; y++) {
        for (i32 x = min_x; x <= max_x; x++) {
            i64 w0 = edge(p0, p1, x, y);
            i64 w1 = edge(p1, p2, x, y);
            i64 w2 = edge(p2, p0, x, y);

            if (!((w0 >= 0 && w1 >= 0 && w2 >= 0) || (w0 <= 0 && w1 <= 0 && w2 <= 0))) {
                continue;
            }

            fb->pixels[(size_t)y * stride_pixels + (size_t)x] = color;
        }
    }
}

void draw_polygon(
    framebuffer_t *fb,
    const draw_point_t *points,
    size_t count,
    pixel_t color
) {
    if (!fb || !fb->pixels || !points || count < 3) {
        return;
    }

    draw_point_t first = points[0];
    for (size_t i = 1; i + 1 < count; i++) {
        draw_triangle(fb, first, points[i], points[i + 1], color);
    }
}
