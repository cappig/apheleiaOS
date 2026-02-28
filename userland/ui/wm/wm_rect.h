#pragma once

#include "wm.h"
#include <stdlib.h>

static inline bool wm_rect_valid(const wm_rect_t *rect) {
    return rect && rect->width > 0 && rect->height > 0;
}

static inline void wm_rect_union(wm_rect_t *dst, const wm_rect_t *src) {
    if (!dst || !wm_rect_valid(src)) {
        return;
    }

    if (!wm_rect_valid(dst)) {
        *dst = *src;
        return;
    }

    i32 x0 = min(dst->x, src->x);
    i32 y0 = min(dst->y, src->y);
    i32 x1 = max(dst->x + dst->width, src->x + src->width);
    i32 y1 = max(dst->y + dst->height, src->y + src->height);

    dst->x = x0;
    dst->y = y0;
    dst->width = x1 - x0;
    dst->height = y1 - y0;
}
