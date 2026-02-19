#include <draw.h>
#include <errno.h>
#include <fcntl.h>
#include <psf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "wm.h"
#include "wm_background.h"
#include "wm_rect.h"

#define BORDER_COLOR    0x00c0c0c0U
#define TITLE_COLOR     0x0020364aU
#define TITLE_FOCUS     0x005fa9d8U
#define CLIENT_BG       0x00101010U
#define TITLE_TEXT      0x00f0f0f0U
#define CLOSE_BG        0x00b04040U
#define CLOSE_FG        0x00ffffffU
#define FONT_BUF_SIZE   (256 * 1024)
#define MAX_TITLE_CHARS 48

static vector_t *windows;
static u8 *font_buf;
static psf_font_t title_font = {0};
static wm_window_t **render_order;
static size_t render_order_capacity;

static int _open_ws_fb(u32 id) {
    char path[64];
    snprintf(path, sizeof(path), "/dev/ws/%u/fb", id);
    return open(path, O_RDWR, 0);
}

static bool _load_font_file(const char *path) {
    return psf_load_file(path, font_buf, FONT_BUF_SIZE, &title_font);
}

static void _init_font(void) {
    if (!font_buf) {
        font_buf = malloc(FONT_BUF_SIZE);
        if (!font_buf) {
            return;
        }
    }

    if (_load_font_file("/boot/font.psf")) {
        return;
    }

    if (_load_font_file("/etc/font.psf")) {
        return;
    }

    memset(&title_font, 0, sizeof(title_font));
}

static void _draw_pixel(u32 *frame, u32 fb_width, u32 fb_height, i32 x, i32 y, u32 color) {
    if (!frame) {
        return;
    }

    if (x < 0 || y < 0 || x >= (i32)fb_width || y >= (i32)fb_height) {
        return;
    }

    frame[(size_t)y * fb_width + (size_t)x] = color;
}

static void
_draw_text(u32 *frame, u32 fb_width, u32 fb_height, i32 x, i32 y, const char *text, u32 color) {
    if (!title_font.glyphs || !text || !text[0]) {
        return;
    }

    i32 pen_x = x;

    for (const char *p = text; *p && (p - text) < MAX_TITLE_CHARS; p++) {
        u32 idx = (u8)(*p);

        if (idx >= title_font.glyph_count) {
            idx = '?';
        }

        const u8 *glyph = title_font.glyphs + (size_t)idx * title_font.glyph_size;

        for (u32 gy = 0; gy < title_font.height; gy++) {
            const u8 *row_ptr = glyph + gy * title_font.row_bytes;

            for (u32 gx = 0; gx < title_font.width; gx++) {
                u8 bits = row_ptr[gx / 8];
                u8 mask = (u8)(0x80 >> (gx & 7));

                if (bits & mask) {
                    _draw_pixel(frame, fb_width, fb_height, pen_x + (i32)gx, y + (i32)gy, color);
                }
            }
        }

        pen_x += (i32)title_font.width;
    }
}

static void _draw_close_button(
    u32 *frame,
    u32 fb_width,
    u32 fb_height,
    const wm_window_t *window,
    bool focused
) {
    if (!window) {
        return;
    }

    i32 bx = window->x + (i32)window->width - CLOSE_BTN_SIZE - 3;
    i32 by = window->y + 3;
    u32 bg = focused ? CLOSE_BG : TITLE_COLOR;

    draw_rect(frame, fb_width, fb_height, bx, by, CLOSE_BTN_SIZE, CLOSE_BTN_SIZE, bg);

    for (i32 i = 2; i < CLOSE_BTN_SIZE - 2; i++) {
        _draw_pixel(frame, fb_width, fb_height, bx + i, by + i, CLOSE_FG);
        _draw_pixel(frame, fb_width, fb_height, bx + i, by + (i32)CLOSE_BTN_SIZE - 1 - i, CLOSE_FG);
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
    }
}

static bool _point_in_rect(i32 px, i32 py, i32 x, i32 y, i32 w, i32 h) {
    return px >= x && py >= y && px < x + w && py < y + h;
}

static bool _point_in_window(const wm_window_t *window, i32 px, i32 py) {
    if (!window) {
        return false;
    }

    return _point_in_rect(
        px, py, window->x, window->y, (i32)window->width, (i32)window->height + TITLE_H
    );
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

static bool _rect_intersect(const wm_rect_t *a, const wm_rect_t *b, wm_rect_t *out) {
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

static bool _window_surface_ensure(wm_window_t *window) {
    if (!window) {
        return false;
    }

    size_t pixels = (size_t)window->width * (size_t)window->height;
    if (window->height && pixels / window->height != window->width) {
        return false;
    }

    if (pixels > ((size_t)-1) / sizeof(u32)) {
        return false;
    }

    if (window->surface && window->surface_pixels == pixels) {
        return true;
    }

    free(window->surface);
    window->surface = NULL;
    window->surface_pixels = 0;

    if (!pixels) {
        return false;
    }

    window->surface = calloc(pixels, sizeof(u32));
    if (!window->surface) {
        return false;
    }

    window->surface_pixels = pixels;
    return true;
}

static void _window_mark_dirty(wm_window_t *window, u32 x, u32 y, u32 width, u32 height) {
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

    u32 x0 = window->dirty_x < x ? window->dirty_x : x;
    u32 y0 = window->dirty_y < y ? window->dirty_y : y;
    u32 x1 = window->dirty_x + window->dirty_width;
    u32 y1 = window->dirty_y + window->dirty_height;
    u32 nx1 = x + width;
    u32 ny1 = y + height;

    if (nx1 > x1) {
        x1 = nx1;
    }
    if (ny1 > y1) {
        y1 = ny1;
    }

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

    if (done < len) {
        memset(dst + done, 0, len - done);
    }

    return true;
}

static bool _window_refresh_surface(wm_window_t *window) {
    if (!window) {
        return false;
    }

    if (!window->surface_dirty) {
        return true;
    }

    if (!_window_surface_ensure(window)) {
        return false;
    }

    u32 x = window->dirty_x;
    u32 y = window->dirty_y;
    u32 width = window->dirty_width;
    u32 height = window->dirty_height;

    window->surface_dirty = false;

    if (!width || !height) {
        return true;
    }

    size_t row_bytes = (size_t)width * sizeof(u32);

    for (u32 row = 0; row < height; row++) {
        u32 src_y = y + row;
        size_t src_index = (size_t)src_y * window->width + x;
        off_t offset = (off_t)(src_index * sizeof(u32));
        u8 *dst = (u8 *)(window->surface + src_index);

        if (window->fb_fd < 0 || !_pread_full(window->fb_fd, dst, row_bytes, offset)) {
            memset(dst, 0, row_bytes);
        }
    }

    return true;
}

static void
_blit_window_region(
    u32 *frame,
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

    wm_rect_t clip_window = {0};
    if (!_rect_intersect(&window_bounds, clip, &clip_window)) {
        return;
    }

    wm_rect_t title_rect = {
        .x = window->x,
        .y = window->y,
        .width = (i32)window->width,
        .height = TITLE_H,
    };

    if (_rect_intersect(&title_rect, clip, NULL)) {
        draw_rect(
            frame,
            fb_width,
            fb_height,
            window->x,
            window->y,
            window->width,
            TITLE_H,
            window->focused ? TITLE_FOCUS : TITLE_COLOR
        );

        _draw_close_button(frame, fb_width, fb_height, window, window->focused);
        if (window->title[0]) {
            _draw_text(
                frame,
                fb_width,
                fb_height,
                window->x + 6,
                window->y + 1,
                window->title,
                TITLE_TEXT
            );
        }
    }

    wm_rect_t client_rect = {
        .x = window->x,
        .y = window->y + TITLE_H,
        .width = (i32)window->width,
        .height = (i32)window->height,
    };

    wm_rect_t clip_client = {0};
    if (!_rect_intersect(&client_rect, clip, &clip_client)) {
        return;
    }

    draw_rect(
        frame, fb_width, fb_height, window->x, window->y + TITLE_H, BORDER_W, window->height, BORDER_COLOR
    );
    draw_rect(
        frame,
        fb_width,
        fb_height,
        window->x + (i32)window->width - BORDER_W,
        window->y + TITLE_H,
        BORDER_W,
        window->height,
        BORDER_COLOR
    );

    if (!_window_refresh_surface(window) || !window->surface) {
        draw_rect(
            frame,
            fb_width,
            fb_height,
            clip_client.x,
            clip_client.y,
            (u32)clip_client.width,
            (u32)clip_client.height,
            CLIENT_BG
        );
        return;
    }

    i32 src_x = clip_client.x - client_rect.x;
    i32 src_y = clip_client.y - client_rect.y;

    if (src_x < 0 || src_y < 0) {
        return;
    }

    size_t copy_pixels = (size_t)clip_client.width;
    size_t copy_bytes = copy_pixels * sizeof(u32);

    for (i32 row = 0; row < clip_client.height; row++) {
        size_t src_off = (size_t)(src_y + row) * window->width + (size_t)src_x;
        size_t dst_off = (size_t)(clip_client.y + row) * fb_width + (size_t)clip_client.x;
        memcpy(frame + dst_off, window->surface + src_off, copy_bytes);
    }
}

void wm_init(void) {
    _init_font();
    windows = vec_create(sizeof(wm_window_t));
}

void wm_destroy(void) {
    vec_destroy(windows);
    windows = NULL;

    if (font_buf) {
        free(font_buf);
        font_buf = NULL;
    }

    free(render_order);
    render_order = NULL;
    render_order_capacity = 0;
}

wm_window_t *wm_window_by_id(u32 id) {
    if (!windows) {
        return NULL;
    }

    for (size_t i = 0; i < windows->size; i++) {
        wm_window_t *window = vec_at(windows, i);
        if (window->id == id) {
            return window;
        }
    }

    return NULL;
}

bool wm_point_in_title(const wm_window_t *window, i32 px, i32 py) {
    if (!window) {
        return false;
    }

    return _point_in_rect(px, py, window->x, window->y, (i32)window->width, TITLE_H);
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

void wm_collect_raise_damage(const wm_window_t *window, u32 old_z, wm_rect_t *damage) {
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
    wm_window_t *top = NULL;

    for (size_t i = 0; i < windows->size; i++) {
        wm_window_t *window = vec_at(windows, i);

        if (!_point_in_window(window, px, py)) {
            continue;
        }

        if (!top || window->z >= top->z) {
            top = window;
        }
    }

    return top;
}

wm_window_t *wm_top_window(void) {
    wm_window_t *top = NULL;

    for (size_t i = 0; i < windows->size; i++) {
        wm_window_t *window = vec_at(windows, i);

        if (!top || window->z >= top->z) {
            top = window;
        }
    }

    return top;
}

void wm_set_focus(ui_t *ui, wm_window_t *window, u32 *z_counter) {
    if (!window) {
        return;
    }

    ui_mgr_focus(ui, window->id);

    if (z_counter) {
        window->z = ++(*z_counter);
        ui_mgr_raise(ui, window->id, window->z);
    }

    for (size_t i = 0; i < windows->size; i++) {
        wm_window_t *w = vec_at(windows, i);
        w->focused = false;
    }

    window->focused = true;
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

        // swap with last element and pop
        size_t idx = ((u8 *)window - (u8 *)windows->data) / windows->elem_size;
        if (idx + 1 < windows->size) {
            wm_window_t *last = vec_at(windows, windows->size - 1);
            memcpy(window, last, sizeof(wm_window_t));
        }

        windows->size--;
        return true;
    }

    if (event->type == WS_EVT_WINDOW_DIRTY) {
        if (!window || !event->width || !event->height) {
            return false;
        }

        _window_mark_dirty(window, (u32)event->x, (u32)event->y, event->width, event->height);

        if (damage) {
            damage->x = window->x + event->x;
            damage->y = window->y + TITLE_H + event->y;
            damage->width = (i32)event->width;
            damage->height = (i32)event->height;
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

        size_t idx = ((u8 *)window - (u8 *)windows->data) / windows->elem_size;
        if (idx + 1 < windows->size) {
            wm_window_t *last = vec_at(windows, windows->size - 1);
            memcpy(window, last, sizeof(wm_window_t));
        }

        windows->size--;
    }

    wm_window_t new_window = {
        .id = event->id,
        .x = event->x,
        .y = event->y,
        .width = event->width,
        .height = event->height,
        .z = event->id,
        .focused = false,
        .fb_fd = _open_ws_fb(event->id),
        .surface = NULL,
        .surface_pixels = 0,
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

    wm_window_t *added = wm_window_by_id(event->id);
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

void wm_render_damage(u32 *frame, u32 fb_width, u32 fb_height, const wm_rect_t *damage) {
    if (!frame || !wm_rect_valid(damage)) {
        return;
    }

    wm_rect_t fb_rect = {.x = 0, .y = 0, .width = (i32)fb_width, .height = (i32)fb_height};
    wm_rect_t clip = {0};
    if (!_rect_intersect(damage, &fb_rect, &clip)) {
        return;
    }

    if (!wm_background_draw_rect(
            frame,
            fb_width,
            fb_height,
            clip.x,
            clip.y,
            (u32)clip.width,
            (u32)clip.height
        )) {
        draw_rect(
            frame, fb_width, fb_height, clip.x, clip.y, (u32)clip.width, (u32)clip.height, BG_COLOR
        );
    }

    size_t count = windows->size;
    if (!count) {
        return;
    }

    if (!_render_order_ensure(count)) {
        for (size_t i = 0; i < count; i++) {
            wm_window_t *window = vec_at(windows, i);
            _blit_window_region(frame, fb_width, fb_height, window, &clip);
        }
        return;
    }

    for (size_t i = 0; i < count; i++) {
        render_order[i] = vec_at(windows, i);
    }

    _sort_by_z(render_order, count);

    for (size_t i = 0; i < count; i++) {
        _blit_window_region(frame, fb_width, fb_height, render_order[i], &clip);
    }
}

void wm_render_frame(u32 *frame, u32 fb_width, u32 fb_height) {
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
}
