#include <draw.h>
#include <data/hashmap.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "wm.h"
#include "background.h"
#include "rect.h"

#define MAX_TITLE_CHARS 48

static vector_t *windows;
static hashmap_t *window_index;

static wm_window_t **render_order;
static size_t render_order_capacity;
static size_t render_order_count;
static bool render_order_dirty = true;

static bool focused_window_valid;
static u32 focused_window_id;

static wm_palette_t wm_palette = {
    .background = 0x00202020U,
    .border = 0x00c0c0c0U,
    .title = 0x0020364aU,
    .title_focus = 0x005fa9d8U,
    .client_bg = 0x00000000U,
    .title_text = 0x00f0f0f0U,
    .close_bg = 0x00b04040U,
    .close_fg = 0x00ffffffU,
};


static u64 _index_encode(size_t index) {
    return (u64)index + 1ULL;
}

static bool _index_decode(u64 encoded, size_t *index_out) {
    if (!encoded || !index_out) {
        return false;
    }

    size_t index = (size_t)(encoded - 1ULL);
    if (_index_encode(index) != encoded) {
        return false;
    }

    *index_out = index;
    return true;
}

static void _window_index_clear(void) {
    if (!window_index) {
        return;
    }

    hashmap_clear(window_index);
}

static void _window_index_remove(u32 id) {
    if (!window_index) {
        return;
    }

    hashmap_remove(window_index, (u64)id);
}

static bool _window_index_set(u32 id, size_t index) {
    if (!window_index) {
        return true;
    }

    return hashmap_set(window_index, (u64)id, _index_encode(index));
}

static bool _window_index_get(u32 id, size_t *index_out) {
    if (!window_index || !index_out) {
        return false;
    }

    u64 encoded = 0;
    if (!hashmap_get(window_index, (u64)id, &encoded)) {
        return false;
    }

    return _index_decode(encoded, index_out);
}

static bool _window_find_index(const wm_window_t *window, size_t *index_out) {
    if (!window || !windows || !index_out || !windows->data || !windows->elem_size) {
        return false;
    }

    const u8 *base = windows->data;
    const u8 *ptr = (const u8 *)window;
    const u8 *end = base + (windows->size * windows->elem_size);

    if (ptr < base || ptr >= end) {
        return false;
    }

    size_t off = (size_t)(ptr - base);
    if (off % windows->elem_size) {
        return false;
    }

    size_t index = off / windows->elem_size;

    if (index >= windows->size) {
        return false;
    }

    *index_out = index;
    return true;
}

static void _window_index_rebuild_from(size_t start) {
    if (!windows || !window_index) {
        return;
    }

    for (size_t i = start; i < windows->size; i++) {
        wm_window_t *window = vec_at(windows, i);
        if (window) {
            _window_index_set(window->id, i);
        }
    }
}

static void _focused_window_revalidate(void) {
    if (!focused_window_valid || !windows) {
        return;
    }

    for (size_t i = 0; i < windows->size; i++) {
        wm_window_t *window = vec_at(windows, i);
        if (window && window->id == focused_window_id) {
            return;
        }
    }

    focused_window_valid = false;
    focused_window_id = 0;
}

static void _window_cache_revalidate(size_t start) {
    _window_index_rebuild_from(start);
    render_order_count = 0;
    render_order_dirty = true;
    _focused_window_revalidate();
}

static void _window_remove_at(size_t index) {
    if (!windows || index >= windows->size) {
        return;
    }

    wm_window_t *slot = vec_at(windows, index);
    if (!slot) {
        return;
    }

    u32 removed_id = slot->id;
    size_t last_index = windows->size - 1;

    if (index != last_index) {
        if (!vec_swap(windows, index, last_index)) {
            return;
        }
    }

    vec_pop(windows, NULL);
    _window_index_remove(removed_id);

    if (focused_window_valid && focused_window_id == removed_id) {
        focused_window_valid = false;
        focused_window_id = 0;
    }

    _window_cache_revalidate(index);
}

static framebuffer_t _wrap_framebuffer(pixel_t *pixels, u32 width, u32 height) {
    framebuffer_t fb = {
        .pixels = pixels,
        .width = width,
        .height = height,
        .stride = width * sizeof(pixel_t),
        .pixel_count = (size_t)width * (size_t)height,
    };
    return fb;
}

void wm_palette_set(const wm_palette_t *palette) {
    if (!palette) {
        return;
    }

    wm_palette = *palette;
}

const wm_palette_t *wm_palette_get(void) {
    return &wm_palette;
}

static int _open_ws_fb(u32 id) {
    char path[64];
    snprintf(path, sizeof(path), "/dev/ws/%u/fb", (unsigned int)id);

    int fd = open(path, O_RDWR, 0);

    if (fd < 0) {
        fprintf(
            stderr,
            "wm: failed to open %s for id=%u errno=%d\n",
            path,
            (unsigned int)id,
            errno
        );
    }

    return fd;
}

static void _draw_pixel(
    pixel_t *frame,
    u32 fb_width,
    u32 fb_height,
    i32 x,
    i32 y,
    pixel_t color
) {
    if (!frame) {
        return;
    }

    if (x < 0 || y < 0 || x >= (i32)fb_width || y >= (i32)fb_height) {
        return;
    }

    frame[(size_t)y * fb_width + (size_t)x] = color;
}

static void _draw_close_button(
    pixel_t *frame,
    u32 fb_width,
    u32 fb_height,
    const wm_window_t *window,
    bool focused
) {
    if (!window) {
        return;
    }

    const wm_palette_t *palette = wm_palette_get();
    i32 bx = window->x + (i32)window->width - CLOSE_BTN_SIZE - 3;
    i32 by = window->y + 3;
    u32 bg = focused ? palette->close_bg : palette->title;
    framebuffer_t fb = _wrap_framebuffer(frame, fb_width, fb_height);

    draw_rect(&fb, bx, by, CLOSE_BTN_SIZE, CLOSE_BTN_SIZE, bg);

    for (i32 i = 2; i < CLOSE_BTN_SIZE - 2; i++) {
        _draw_pixel(
            frame, fb_width, fb_height, bx + i, by + i, palette->close_fg
        );
        _draw_pixel(
            frame,
            fb_width,
            fb_height,
            bx + i,
            by + (i32)CLOSE_BTN_SIZE - 1 - i,
            palette->close_fg
        );
    }
}

static void _cleanup_window(wm_window_t *window) {
    if (!window) {
        return;
    }

    if (window->fb_fd >= 0) {
        close(window->fb_fd);
        window->fb_fd = -1;
    }

    if (window->surface) {
        free(window->surface);
        window->surface = NULL;
        window->surface_pixels = 0;
        window->surface_capacity = 0;
    }

    window->surface_width = 0;
    window->surface_height = 0;
}

static bool _point_in_rect(i32 px, i32 py, i32 x, i32 y, i32 w, i32 h) {
    return px >= x && py >= y && px < x + w && py < y + h;
}

static bool _point_in_window(const wm_window_t *window, i32 px, i32 py) {
    if (!window) {
        return false;
    }

    return _point_in_rect(
        px,
        py,
        window->x,
        window->y,
        (i32)window->width,
        (i32)window->height + TITLE_H
    );
}

static bool _window_title_rect(const wm_window_t *window, wm_rect_t *rect) {
    if (!window || !rect) {
        return false;
    }

    rect->x = window->x;
    rect->y = window->y;
    rect->width = (i32)window->width;
    rect->height = TITLE_H;
    return wm_rect_valid(rect);
}

static bool _window_client_rect(const wm_window_t *window, wm_rect_t *rect) {
    if (!window || !rect) {
        return false;
    }

    rect->x = window->x;
    rect->y = window->y + TITLE_H;
    rect->width = (i32)window->width;
    rect->height = (i32)window->height;
    return wm_rect_valid(rect);
}

static int _compare_window_z(const void *lhs, const void *rhs) {
    if (!lhs || !rhs) {
        return 0;
    }

    const wm_window_t *const *left = lhs;
    const wm_window_t *const *right = rhs;

    if (!*left || !*right) {
        return 0;
    }

    if ((*left)->z < (*right)->z) {
        return -1;
    }

    if ((*left)->z > (*right)->z) {
        return 1;
    }

    if ((*left)->id < (*right)->id) {
        return -1;
    }

    if ((*left)->id > (*right)->id) {
        return 1;
    }

    return 0;
}

static void _sort_by_z(wm_window_t **arr, size_t count) {
    if (!arr || count < 2) {
        return;
    }

    qsort(arr, count, sizeof(arr[0]), _compare_window_z);
}

static bool _render_order_ensure(size_t needed) {
    if (render_order_capacity >= needed) {
        return true;
    }

    size_t new_capacity = render_order_capacity ? render_order_capacity : 8;
    while (new_capacity < needed) {
        size_t grown = new_capacity * 2;
        if (grown <= new_capacity) {
            return false;
        }
        new_capacity = grown;
    }

    wm_window_t **grown = realloc(render_order, new_capacity * sizeof(*grown));
    if (!grown) {
        return false;
    }

    render_order = grown;
    render_order_capacity = new_capacity;
    return true;
}

static bool _render_order_refresh(void) {
    size_t count = windows ? windows->size : 0;
    if (!count) {
        render_order_count = 0;
        render_order_dirty = false;
        return true;
    }

    if (!_render_order_ensure(count)) {
        return false;
    }

    if (!render_order_dirty && render_order_count == count) {
        return true;
    }

    for (size_t i = 0; i < count; i++) {
        render_order[i] = vec_at(windows, i);
    }

    _sort_by_z(render_order, count);
    render_order_count = count;
    render_order_dirty = false;
    return true;
}

static wm_window_t *_top_window_linear(bool at_point, i32 px, i32 py) {
    wm_window_t *top = NULL;
    if (!windows) {
        return NULL;
    }

    for (size_t i = 0; i < windows->size; i++) {
        wm_window_t *window = vec_at(windows, i);
        if (at_point && !_point_in_window(window, px, py)) {
            continue;
        }

        if (!top || window->z >= top->z) {
            top = window;
        }
    }

    return top;
}

static bool
_rect_intersect(const wm_rect_t *a, const wm_rect_t *b, wm_rect_t *out) {
    if (!wm_rect_valid(a) || !wm_rect_valid(b)) {
        if (out) {
            memset(out, 0, sizeof(*out));
        }
        return false;
    }

    i32 ax0 = a->x;
    i32 ay0 = a->y;
    i32 ax1 = a->x + a->width;
    i32 ay1 = a->y + a->height;

    i32 bx0 = b->x;
    i32 by0 = b->y;
    i32 bx1 = b->x + b->width;
    i32 by1 = b->y + b->height;

    i32 x0 = ax0 > bx0 ? ax0 : bx0;
    i32 y0 = ay0 > by0 ? ay0 : by0;
    i32 x1 = ax1 < bx1 ? ax1 : bx1;
    i32 y1 = ay1 < by1 ? ay1 : by1;

    if (x0 >= x1 || y0 >= y1) {
        if (out) {
            memset(out, 0, sizeof(*out));
        }
        return false;
    }

    if (out) {
        out->x = x0;
        out->y = y0;
        out->width = x1 - x0;
        out->height = y1 - y0;
    }

    return true;
}

static void _surface_resize_preserve_in_place(
    pixel_t *surface,
    u32 old_width,
    u32 old_height,
    u32 new_width,
    u32 new_height
) {
    if (!surface || !new_width || !new_height || !old_width || !old_height) {
        return;
    }

    u32 copy_width = old_width < new_width ? old_width : new_width;
    u32 copy_height = old_height < new_height ? old_height : new_height;
    size_t copy_bytes = (size_t)copy_width * sizeof(pixel_t);

    if (new_width > old_width) {
        for (u32 row = copy_height; row > 0; row--) {
            size_t i = (size_t)(row - 1);
            pixel_t *dst = surface + (i * new_width);
            const pixel_t *src = surface + (i * old_width);
            memmove(dst, src, copy_bytes);
        }
    } else if (new_width < old_width) {
        for (u32 row = 0; row < copy_height; row++) {
            size_t i = (size_t)row;
            pixel_t *dst = surface + (i * new_width);
            const pixel_t *src = surface + (i * old_width);
            memmove(dst, src, copy_bytes);
        }
    }

    if (new_width > old_width) {
        for (u32 row = 0; row < copy_height; row++) {
            size_t row_off = (size_t)row * new_width;
            pixel_t fill = surface[row_off + (old_width - 1)];

            for (u32 col = old_width; col < new_width; col++) {
                surface[row_off + col] = fill;
            }
        }
    }

    if (new_height > old_height) {
        size_t src_row = (size_t)(old_height - 1) * new_width;
        for (u32 row = old_height; row < new_height; row++) {
            size_t row_off = (size_t)row * new_width;
            memcpy(
                surface + row_off,
                surface + src_row,
                (size_t)new_width * sizeof(pixel_t)
            );
        }
    }
}

static void _surface_copy_overlap(
    pixel_t *dst,
    u32 dst_width,
    u32 dst_height,
    const pixel_t *src,
    u32 src_width,
    u32 src_height
) {
    if (!dst || !src || !dst_width || !dst_height || !src_width || !src_height) {
        return;
    }

    u32 copy_width = src_width < dst_width ? src_width : dst_width;
    u32 copy_height = src_height < dst_height ? src_height : dst_height;
    size_t row_bytes = (size_t)copy_width * sizeof(pixel_t);

    for (u32 row = 0; row < copy_height; row++) {
        memcpy(
            dst + (size_t)row * dst_width,
            src + (size_t)row * src_width,
            row_bytes
        );
    }
}

static bool _window_surface_ensure(wm_window_t *window) {
    if (!window) {
        return false;
    }

    u32 old_width = window->surface_width;
    u32 old_height = window->surface_height;
    u32 new_width = window->fb_width ? window->fb_width : window->width;
    u32 new_height = window->fb_height ? window->fb_height : window->height;

    size_t pixels = (size_t)new_width * (size_t)new_height;
    if (new_height && pixels / new_height != new_width) {
        fprintf(
            stderr,
            "wm: surface size overflow id=%u dims=%ux%u\n",
            (unsigned int)window->id,
            (unsigned int)new_width,
            (unsigned int)new_height
        );
        return false;
    }

    if (pixels > ((size_t)-1) / sizeof(pixel_t)) {
        fprintf(
            stderr,
            "wm: surface byte overflow id=%u pixels=%zu\n",
            (unsigned int)window->id,
            pixels
        );
        return false;
    }

    if (window->surface && window->surface_capacity >= pixels) {
        window->surface_pixels = pixels;
        if (old_width != new_width || old_height != new_height) {
            _surface_resize_preserve_in_place(
                window->surface, old_width, old_height, new_width, new_height
            );
        }
        window->surface_width = new_width;
        window->surface_height = new_height;
        return true;
    }

    size_t new_capacity = pixels;

    if (new_capacity > ((size_t)-1) / sizeof(pixel_t)) {
        fprintf(
            stderr,
            "wm: surface capacity overflow id=%u capacity=%zu\n",
            (unsigned int)window->id,
            new_capacity
        );
        return false;
    }

    pixel_t *surface = calloc(new_capacity, sizeof(pixel_t));
    if (!surface) {
        fprintf(
            stderr,
            "wm: surface allocation failed id=%u capacity=%zu bytes=%zu\n",
            (unsigned int)window->id,
            new_capacity,
            new_capacity * sizeof(pixel_t)
        );
        return false;
    }

    if (window->surface && window->surface_pixels && old_width && old_height) {
        _surface_copy_overlap(
            surface,
            new_width,
            new_height,
            window->surface,
            old_width,
            old_height
        );
    }

    free(window->surface);
    window->surface = surface;
    window->surface_capacity = new_capacity;
    window->surface_pixels = pixels;
    window->surface_width = new_width;
    window->surface_height = new_height;
    return true;
}

static void
_window_mark_dirty(wm_window_t *window, u32 x, u32 y, u32 width, u32 height) {
    if (!window || !width || !height || !window->width || !window->height) {
        return;
    }

    if (x >= window->width || y >= window->height) {
        return;
    }

    if (x + width > window->width) {
        width = window->width - x;
    }

    if (y + height > window->height) {
        height = window->height - y;
    }

    if (!width || !height) {
        return;
    }

    if (!window->surface_dirty) {
        window->dirty_x = x;
        window->dirty_y = y;
        window->dirty_width = width;
        window->dirty_height = height;
        window->surface_dirty = true;
        return;
    }

    u32 x0 = min(window->dirty_x, x);
    u32 y0 = min(window->dirty_y, y);
    u32 x1 = max(window->dirty_x + window->dirty_width, x + width);
    u32 y1 = max(window->dirty_y + window->dirty_height, y + height);

    window->dirty_x = x0;
    window->dirty_y = y0;
    window->dirty_width = x1 - x0;
    window->dirty_height = y1 - y0;
}

static bool _pread_full(int fd, void *buf, size_t len, off_t offset) {
    if (fd < 0 || !buf || !len) {
        return false;
    }

    u8 *dst = buf;
    size_t done = 0;

    while (done < len) {
        ssize_t n = pread(fd, dst + done, len - done, offset + (off_t)done);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }

            return false;
        }

        if (!n) {
            break;
        }

        done += (size_t)n;
    }

    return done == len;
}

static bool _window_refresh_surface(wm_window_t *window) {
    if (!window) {
        return false;
    }

    if (!window->surface_dirty) {
        return true;
    }

    if (!_window_surface_ensure(window)) {
        window->fb_read_failures++;
        if (
            window->fb_read_failures <= 4 ||
            (window->fb_read_failures & (window->fb_read_failures - 1U)) == 0U
        ) {
            fprintf(
                stderr,
                "wm: surface ensure failed id=%u failures=%u dims=%ux%u\n",
                (unsigned int)window->id,
                (unsigned int)window->fb_read_failures,
                (unsigned int)(window->fb_width ? window->fb_width : window->width),
                (unsigned int)(window->fb_height ? window->fb_height : window->height)
            );
        }
        return false;
    }

    u32 x = window->dirty_x;
    u32 y = window->dirty_y;
    u32 width = window->dirty_width;
    u32 height = window->dirty_height;
    u32 src_width = window->fb_width ? window->fb_width : window->width;
    u32 src_height = window->fb_height ? window->fb_height : window->height;
    u32 surface_width =
        window->surface_width ? window->surface_width : window->width;

    window->surface_dirty = false;

    if (!width || !height || !src_width || !src_height) {
        return true;
    }

    if (x >= src_width || y >= src_height) {
        return true;
    }

    if (x + width > src_width) {
        width = src_width - x;
    }

    if (y + height > src_height) {
        height = src_height - y;
    }

    if (!width || !height) {
        return true;
    }

    size_t row_bytes = (size_t)width * sizeof(pixel_t);
    bool had_error = false;

    if (window->fb_fd >= 0 && x == 0 && width == src_width && surface_width == src_width) {
        size_t src_index = (size_t)y * src_width;
        size_t dst_index = (size_t)y * surface_width;
        off_t offset = (off_t)(src_index * sizeof(pixel_t));
        size_t total = (size_t)height * row_bytes;
        u8 *dst = (u8 *)(window->surface + dst_index);

        if (!_pread_full(window->fb_fd, dst, total, offset)) {
            had_error = true;
        }
    } else {
        for (u32 row = 0; row < height; row++) {
            u32 src_y = y + row;
            size_t src_index = (size_t)src_y * src_width + x;
            size_t dst_index = (size_t)src_y * surface_width + x;
            off_t offset = (off_t)(src_index * sizeof(pixel_t));
            u8 *dst = (u8 *)(window->surface + dst_index);

            if (window->fb_fd < 0 || !_pread_full(window->fb_fd, dst, row_bytes, offset)) {
                had_error = true;
            }
        }
    }

    if (had_error) {
        window->fb_read_failures++;
        if (
            window->fb_read_failures <= 4 ||
            (window->fb_read_failures & (window->fb_read_failures - 1U)) == 0U
        ) {
            fprintf(
                stderr,
                "wm: fb read failed id=%u failures=%u rect=%u,%u %ux%u src=%ux%u\n",
                (unsigned int)window->id,
                (unsigned int)window->fb_read_failures,
                (unsigned int)x,
                (unsigned int)y,
                (unsigned int)width,
                (unsigned int)height,
                (unsigned int)src_width,
                (unsigned int)src_height
            );
        }
        _window_mark_dirty(window, x, y, width, height);
    } else if (window->fb_read_failures) {
        fprintf(
            stderr,
            "wm: fb read recovered id=%u after_failures=%u\n",
            (unsigned int)window->id,
            (unsigned int)window->fb_read_failures
        );
        window->fb_read_failures = 0;
    }

    return true;
}

static void _draw_window_title_region(
    pixel_t *frame,
    u32 fb_width,
    u32 fb_height,
    const wm_window_t *window,
    const wm_rect_t *clip,
    const wm_palette_t *palette
) {
    if (!frame || !window || !wm_rect_valid(clip) || !palette) {
        return;
    }

    wm_rect_t title_rect = {0};
    if (!_window_title_rect(window, &title_rect)) {
        return;
    }

    wm_rect_t clip_title = {0};
    if (!_rect_intersect(&title_rect, clip, &clip_title)) {
        return;
    }

    framebuffer_t fb = _wrap_framebuffer(frame, fb_width, fb_height);
    draw_rect(
        &fb,
        title_rect.x,
        title_rect.y,
        (u32)title_rect.width,
        (u32)title_rect.height,
        window->focused ? palette->title_focus : palette->title
    );

    wm_rect_t close_rect = {
        .x = window->x + (i32)window->width - CLOSE_BTN_SIZE - 3,
        .y = window->y + 3,
        .width = CLOSE_BTN_SIZE,
        .height = CLOSE_BTN_SIZE,
    };
    if (_rect_intersect(&close_rect, &clip_title, NULL)) {
        _draw_close_button(frame, fb_width, fb_height, window, window->focused);
    }

    u32 title_font_w = draw_font_width();
    u32 title_font_h = draw_font_height();
    if (!window->title[0] || !title_font_w || !title_font_h) {
        return;
    }

    size_t title_len = strnlen(window->title, MAX_TITLE_CHARS);
    if (!title_len) {
        return;
    }

    char title_text[MAX_TITLE_CHARS + 1];
    memcpy(title_text, window->title, title_len);
    title_text[title_len] = '\0';

    i32 text_y = title_rect.y + ((title_rect.height - (i32)title_font_h) / 2);
    if (text_y < title_rect.y) {
        text_y = title_rect.y;
    }

    wm_rect_t text_rect = {
        .x = window->x + 6,
        .y = text_y,
        .width = (i32)(title_len * title_font_w),
        .height = (i32)title_font_h,
    };
    if (_rect_intersect(&text_rect, &clip_title, NULL)) {
        draw_text(
            &fb,
            text_rect.x,
            text_rect.y,
            title_text,
            palette->title_text
        );
    }
}

static bool _copy_window_client_region(
    pixel_t *frame,
    u32 fb_width,
    const wm_window_t *window,
    const wm_rect_t *client_rect,
    const wm_rect_t *clip_client,
    u32 *copy_width_out,
    u32 *copy_height_out
) {
    if (
        !frame ||
        !window ||
        !window->surface ||
        !client_rect ||
        !clip_client ||
        !copy_width_out ||
        !copy_height_out
    ) {
        return false;
    }

    i32 src_x = clip_client->x - client_rect->x;
    i32 src_y = clip_client->y - client_rect->y;
    if (src_x < 0 || src_y < 0) {
        return false;
    }

    u32 surface_stride = window->surface_width;
    u32 surface_rows = window->surface_height;
    if (!surface_stride || !surface_rows) {
        return false;
    }

    u32 valid_width = window->fb_width ? window->fb_width : window->width;
    u32 valid_height = window->fb_height ? window->fb_height : window->height;
    if (valid_width > surface_stride) {
        valid_width = surface_stride;
    }

    if (valid_height > surface_rows) {
        valid_height = surface_rows;
    }

    if ((u64)surface_stride * (u64)surface_rows > window->surface_pixels) {
        u32 max_rows = (u32)(window->surface_pixels / surface_stride);
        if (!max_rows) {
            return false;
        }

        surface_rows = max_rows;
        if (valid_height > surface_rows) {
            valid_height = surface_rows;
        }
    }

    u32 copy_width = 0;
    u32 copy_height = 0;

    if ((u32)src_x < valid_width && (u32)src_y < valid_height) {
        copy_width = valid_width - (u32)src_x;

        if (copy_width > (u32)clip_client->width) {
            copy_width = (u32)clip_client->width;
        }

        copy_height = valid_height - (u32)src_y;
        if (copy_height > (u32)clip_client->height) {
            copy_height = (u32)clip_client->height;
        }
    }

    if (copy_width && copy_height) {
        size_t copy_bytes = (size_t)copy_width * sizeof(pixel_t);
        for (u32 row = 0; row < copy_height; row++) {
            size_t src_off =
                (size_t)(src_y + (i32)row) * surface_stride + (size_t)src_x;

            size_t dst_off =
                (size_t)(clip_client->y + (i32)row) * fb_width + (size_t)clip_client->x;

            memcpy(frame + dst_off, window->surface + src_off, copy_bytes);
        }
    }

    *copy_width_out = copy_width;
    *copy_height_out = copy_height;

    return true;
}

static void _fill_window_client_gap(
    framebuffer_t *fb,
    const wm_rect_t *clip_client,
    u32 copy_width,
    u32 copy_height
) {
    if (!fb || !clip_client) {
        return;
    }

    u32 clip_w = (u32)clip_client->width;
    u32 clip_h = (u32)clip_client->height;

    if (!clip_w || !clip_h || !fb->pixels) {
        return;
    }

    if (clip_client->x < 0 || clip_client->y < 0) {
        const wm_palette_t *palette = wm_palette_get();
        pixel_t fill = palette ? palette->client_bg : 0;
        draw_rect(
            fb,
            clip_client->x,
            clip_client->y,
            clip_w,
            clip_h,
            fill
        );
        return;
    }

    if (copy_width > clip_w) {
        copy_width = clip_w;
    }
    if (copy_height > clip_h) {
        copy_height = clip_h;
    }

    if (!copy_width || !copy_height) {
        const wm_palette_t *palette = wm_palette_get();
        pixel_t fill = palette ? palette->client_bg : 0;
        draw_rect(
            fb,
            clip_client->x,
            clip_client->y,
            clip_w,
            clip_h,
            fill
        );
        return;
    }

    size_t stride_pixels = fb->stride / sizeof(pixel_t);
    if (!stride_pixels) {
        stride_pixels = fb->width;
    }

    size_t base_x = (size_t)clip_client->x;
    size_t base_y = (size_t)clip_client->y;

    if (base_x >= fb->width || base_y >= fb->height) {
        return;
    }

    size_t max_w = (size_t)fb->width - base_x;
    size_t max_h = (size_t)fb->height - base_y;

    if ((size_t)clip_w > max_w) {
        clip_w = (u32)max_w;
    }

    if ((size_t)clip_h > max_h) {
        clip_h = (u32)max_h;
    }

    if (!clip_w || !clip_h) {
        return;
    }

    if (copy_width < clip_w) {
        size_t edge_x = base_x + (size_t)copy_width - 1;

        for (u32 row = 0; row < copy_height; row++) {
            size_t y = base_y + (size_t)row;

            pixel_t fill = fb->pixels[y * stride_pixels + edge_x];
            pixel_t *dst = fb->pixels + y * stride_pixels + base_x + copy_width;

            for (u32 col = copy_width; col < clip_w; col++) {
                *dst++ = fill;
            }
        }
    }

    if (copy_height < clip_h) {
        size_t src_y = base_y + (size_t)copy_height - 1;

        const pixel_t *src = fb->pixels + src_y * stride_pixels + base_x;
        size_t row_bytes = (size_t)clip_w * sizeof(pixel_t);

        for (u32 row = copy_height; row < clip_h; row++) {
            size_t y = base_y + (size_t)row;
            pixel_t *dst = fb->pixels + y * stride_pixels + base_x;
            memcpy(dst, src, row_bytes);
        }
    }
}

static void _draw_window_client_region(
    pixel_t *frame,
    u32 fb_width,
    u32 fb_height,
    wm_window_t *window,
    const wm_rect_t *clip,
    const wm_palette_t *palette
) {
    if (!frame || !window || !wm_rect_valid(clip) || !palette) {
        return;
    }

    framebuffer_t fb = _wrap_framebuffer(frame, fb_width, fb_height);
    wm_rect_t client_rect = {0};
    if (!_window_client_rect(window, &client_rect)) {
        return;
    }

    wm_rect_t clip_client = {0};
    if (!_rect_intersect(&client_rect, clip, &clip_client)) {
        return;
    }

    draw_rect(
        &fb,
        window->x,
        window->y + TITLE_H,
        BORDER_W,
        window->height,
        palette->border
    );

    draw_rect(
        &fb,
        window->x + (i32)window->width - BORDER_W,
        window->y + TITLE_H,
        BORDER_W,
        window->height,
        palette->border
    );

    if (!_window_refresh_surface(window) || !window->surface) {
        draw_rect(
            &fb,
            clip_client.x,
            clip_client.y,
            (u32)clip_client.width,
            (u32)clip_client.height,
            palette->client_bg
        );
        return;
    }

    u32 copy_width = 0;
    u32 copy_height = 0;

    bool copied_region = _copy_window_client_region(
        frame,
        fb_width,
        window,
        &client_rect,
        &clip_client,
        &copy_width,
        &copy_height
    );

    if (!copied_region) {
        draw_rect(
            &fb,
            clip_client.x,
            clip_client.y,
            (u32)clip_client.width,
            (u32)clip_client.height,
            palette->client_bg
        );
        return;
    }

    _fill_window_client_gap(
        &fb, &clip_client, copy_width, copy_height
    );
}

static void _blit_window_region(
    pixel_t *frame,
    u32 fb_width,
    u32 fb_height,
    wm_window_t *window,
    const wm_rect_t *clip
) {
    if (!frame || !window || !wm_rect_valid(clip)) {
        return;
    }

    wm_rect_t window_bounds = {0};
    if (!wm_window_bounds_rect(window, &window_bounds)) {
        return;
    }

    if (!_rect_intersect(&window_bounds, clip, NULL)) {
        return;
    }

    const wm_palette_t *palette = wm_palette_get();
    _draw_window_title_region(
        frame, fb_width, fb_height, window, clip, palette
    );
    _draw_window_client_region(
        frame, fb_width, fb_height, window, clip, palette
    );
}

void wm_init(void) {
    windows = vec_create(sizeof(wm_window_t));
    window_index = hashmap_create();
    render_order_count = 0;
    render_order_dirty = true;
    focused_window_valid = false;
    focused_window_id = 0;
}

void wm_destroy(void) {
    vec_destroy(windows);
    windows = NULL;

    hashmap_destroy(window_index);
    window_index = NULL;

    free(render_order);
    render_order = NULL;
    render_order_capacity = 0;
    render_order_count = 0;
    render_order_dirty = true;
    focused_window_valid = false;
    focused_window_id = 0;
}

wm_window_t *wm_window_by_id(u32 id) {
    if (!windows) {
        return NULL;
    }

    size_t cached_index = 0;
    if (_window_index_get(id, &cached_index) && cached_index < windows->size) {
        wm_window_t *cached = vec_at(windows, cached_index);

        if (cached && cached->id == id) {
            return cached;
        }

        _window_index_remove(id);
    }

    for (size_t i = 0; i < windows->size; i++) {
        wm_window_t *window = vec_at(windows, i);

        if (window->id == id) {
            _window_index_set(id, i);
            return window;
        }
    }

    return NULL;
}

bool wm_point_in_title(const wm_window_t *window, i32 px, i32 py) {
    if (!window) {
        return false;
    }

    return _point_in_rect(
        px, py, window->x, window->y, (i32)window->width, TITLE_H
    );
}

bool wm_point_in_close(const wm_window_t *window, i32 px, i32 py) {
    if (!window) {
        return false;
    }

    i32 bx = window->x + (i32)window->width - CLOSE_BTN_SIZE - 3;
    i32 by = window->y + 3;

    return _point_in_rect(px, py, bx, by, CLOSE_BTN_SIZE, CLOSE_BTN_SIZE);
}

bool wm_window_bounds_rect(const wm_window_t *window, wm_rect_t *rect) {
    if (!window || !rect) {
        return false;
    }

    rect->x = window->x;
    rect->y = window->y;
    rect->width = (i32)window->width;
    rect->height = TITLE_H + (i32)window->height;

    return rect->width > 0 && rect->height > 0;
}

void wm_collect_raise_damage(
    const wm_window_t *window,
    u32 old_z,
    wm_rect_t *damage
) {
    if (!window || !damage || !windows) {
        return;
    }

    wm_rect_t target = {0};
    if (!wm_window_bounds_rect(window, &target)) {
        return;
    }

    for (size_t i = 0; i < windows->size; i++) {
        wm_window_t *other = vec_at(windows, i);

        if (!other || other == window) {
            continue;
        }

        if (other->z <= old_z) {
            continue;
        }

        wm_rect_t other_rect = {0};
        if (!wm_window_bounds_rect(other, &other_rect)) {
            continue;
        }

        wm_rect_t intersection = {0};
        if (_rect_intersect(&target, &other_rect, &intersection)) {
            wm_rect_union(damage, &intersection);
        }
    }
}

wm_window_t *wm_top_window_at(i32 px, i32 py) {
    if (!windows || !windows->size) {
        return NULL;
    }

    if (_render_order_refresh()) {
        for (size_t i = render_order_count; i > 0; i--) {
            wm_window_t *window = render_order[i - 1];

            if (_point_in_window(window, px, py)) {
                return window;
            }
        }

        return NULL;
    }

    return _top_window_linear(true, px, py);
}

wm_window_t *wm_top_window(void) {
    if (!windows || !windows->size) {
        return NULL;
    }

    if (_render_order_refresh()) {
        return render_order_count ? render_order[render_order_count - 1] : NULL;
    }

    return _top_window_linear(false, 0, 0);
}

void wm_set_focus(ui_t *ui, wm_window_t *window, u32 *z_counter) {
    if (!window) {
        return;
    }

    ui_mgr_focus(ui, window->id);

    if (z_counter) {
        window->z = ++(*z_counter);
        ui_mgr_raise(ui, window->id, window->z);
        render_order_dirty = true;
    }

    if (focused_window_valid && focused_window_id != window->id) {
        size_t focused_index = 0;

        if (_window_index_get(focused_window_id, &focused_index) && focused_index < windows->size) {
            wm_window_t *focused = vec_at(windows, focused_index);

            if (focused && focused->id == focused_window_id) {
                focused->focused = false;
            } else {
                _window_index_remove(focused_window_id);
                focused_window_valid = false;
            }
        } else {
            focused_window_valid = false;
        }
    }

    window->focused = true;
    focused_window_valid = true;
    focused_window_id = window->id;
}

bool wm_handle_ws_event(const ws_event_t *event, wm_rect_t *damage) {
    if (!event) {
        return false;
    }

    if (damage) {
        memset(damage, 0, sizeof(*damage));
    }

    wm_window_t *window = wm_window_by_id(event->id);

    if (event->type == WS_EVT_WINDOW_CLOSED) {
        if (!window) {
            return false;
        }

        if (damage) {
            wm_window_bounds_rect(window, damage);
        }

        _cleanup_window(window);

        size_t index = 0;
        if (!_window_find_index(window, &index)) {
            _window_index_remove(event->id);
            return false;
        }

        _window_remove_at(index);
        render_order_dirty = true;

        return true;
    }

    if (event->type == WS_EVT_WINDOW_DIRTY) {
        if (!window || !event->width || !event->height) {
            return false;
        }

        _window_mark_dirty(
            window, (u32)event->x, (u32)event->y, event->width, event->height
        );

        if (damage) {
            damage->x = window->x + event->x;
            damage->y = window->y + TITLE_H + event->y;
            damage->width = (i32)event->width;
            damage->height = (i32)event->height;
        }

        return true;
    }

    if (event->type == WS_EVT_WINDOW_TITLE) {
        if (!window) {
            return false;
        }

        if (!strncmp(window->title, event->title, sizeof(window->title))) {
            return false;
        }

        memset(window->title, 0, sizeof(window->title));
        strncpy(window->title, event->title, sizeof(window->title) - 1);

        if (damage) {
            return _window_title_rect(window, damage);
        }

        return true;
    }

    if (event->type != WS_EVT_WINDOW_NEW) {
        return false;
    }

    wm_rect_t old_rect = {0};
    bool had_old = false;

    // if a window with this id already exists, clean it first
    if (window) {
        had_old = wm_window_bounds_rect(window, &old_rect);
        _cleanup_window(window);

        size_t index = 0;
        if (_window_find_index(window, &index)) {
            _window_remove_at(index);
        } else {
            _window_index_remove(event->id);
        }
    }

    wm_window_t new_window = {
        .id = event->id,
        .x = event->x,
        .y = event->y,
        .width = event->width,
        .height = event->height,
        .fb_width = event->width,
        .fb_height = event->height,
        .z = event->id,
        .focused = false,
        .fb_fd = _open_ws_fb(event->id),
        .surface = NULL,
        .surface_pixels = 0,
        .surface_capacity = 0,
        .surface_width = 0,
        .surface_height = 0,
        .surface_dirty = false,
        .dirty_x = 0,
        .dirty_y = 0,
        .dirty_width = 0,
        .dirty_height = 0,
        .title = {0},
    };

    strncpy(new_window.title, event->title, sizeof(new_window.title) - 1);
    if (!vec_push(windows, &new_window)) {
        _cleanup_window(&new_window);
        return false;
    }

    size_t added_index = windows->size - 1;
    _window_cache_revalidate(added_index);

    wm_window_t *added = vec_at(windows, added_index);
    if (added) {
        _window_mark_dirty(added, 0, 0, added->width, added->height);
    }

    if (damage) {
        wm_rect_t new_rect = {0};
        if (added && wm_window_bounds_rect(added, &new_rect)) {
            *damage = new_rect;
        }

        if (had_old) {
            wm_rect_union(damage, &old_rect);
        }
    }

    return true;
}

void wm_render_damage(
    pixel_t *frame,
    u32 fb_width,
    u32 fb_height,
    const wm_rect_t *damage
) {
    if (!frame || !wm_rect_valid(damage)) {
        return;
    }

    const wm_palette_t *palette = wm_palette_get();
    framebuffer_t fb = _wrap_framebuffer(frame, fb_width, fb_height);

    wm_rect_t fb_rect = {
        .x = 0, .y = 0, .width = (i32)fb_width, .height = (i32)fb_height
    };
    wm_rect_t clip = {0};
    if (!_rect_intersect(damage, &fb_rect, &clip)) {
        return;
    }

    bool background_drawn = wm_background_draw_rect(
        frame,
        fb_width,
        fb_height,
        clip.x,
        clip.y,
        (u32)clip.width,
        (u32)clip.height
    );

    if (!background_drawn) {
        draw_rect(
            &fb,
            clip.x,
            clip.y,
            (u32)clip.width,
            (u32)clip.height,
            palette->background
        );
    }

    size_t count = windows->size;
    if (!count) {
        return;
    }

    if (!_render_order_refresh()) {
        for (size_t i = 0; i < count; i++) {
            wm_window_t *window = vec_at(windows, i);
            _blit_window_region(frame, fb_width, fb_height, window, &clip);
        }
        return;
    }

    for (size_t i = 0; i < render_order_count; i++) {
        _blit_window_region(frame, fb_width, fb_height, render_order[i], &clip);
    }
}

void wm_render_frame(pixel_t *frame, u32 fb_width, u32 fb_height) {
    wm_rect_t full = {
        .x = 0,
        .y = 0,
        .width = (i32)fb_width,
        .height = (i32)fb_height,
    };
    wm_render_damage(frame, fb_width, fb_height, &full);
}

void wm_cleanup_all_windows(void) {
    for (size_t i = 0; i < windows->size; i++) {
        wm_window_t *window = vec_at(windows, i);
        _cleanup_window(window);
    }

    vec_clear(windows);
    _window_index_clear();

    render_order_count = 0;
    render_order_dirty = true;
    focused_window_valid = false;
    focused_window_id = 0;
}
