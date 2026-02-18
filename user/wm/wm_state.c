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
static u32 *blit_buf;
static size_t blit_buf_cap;
static u8 *font_buf;
static psf_font_t title_font = {0};

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

static bool _ensure_blit_buf(size_t pixels) {
    if (pixels <= blit_buf_cap) {
        return true;
    }

    u32 *buf = realloc(blit_buf, pixels * sizeof(u32));
    if (!buf) {
        return false;
    }

    blit_buf = buf;
    blit_buf_cap = pixels;
    return true;
}

static void _blit_window(u32 *frame, u32 fb_width, u32 fb_height, wm_window_t *window) {
    if (!frame || !window) {
        return;
    }

    i32 x = window->x;
    i32 y = window->y;
    u32 w = window->width;
    u32 h = window->height;

    draw_rect(
        frame, fb_width, fb_height, x, y, w, TITLE_H, window->focused ? TITLE_FOCUS : TITLE_COLOR
    );
    draw_rect(frame, fb_width, fb_height, x, y + TITLE_H, BORDER_W, h, BORDER_COLOR);
    draw_rect(
        frame, fb_width, fb_height, x + (i32)w - BORDER_W, y + TITLE_H, BORDER_W, h, BORDER_COLOR
    );

    _draw_close_button(frame, fb_width, fb_height, window, window->focused);

    if (window->title[0]) {
        _draw_text(frame, fb_width, fb_height, x + 6, y + 1, window->title, TITLE_TEXT);
    }

    if (window->fb_fd < 0) {
        draw_rect(
            frame,
            fb_width,
            fb_height,
            x + BORDER_W,
            y + TITLE_H + BORDER_W,
            w - 2 * BORDER_W,
            h - 2 * BORDER_W,
            CLIENT_BG
        );
        return;
    }

    size_t total_pixels = (size_t)w * (size_t)h;

    if (!_ensure_blit_buf(total_pixels)) {
        draw_rect(
            frame,
            fb_width,
            fb_height,
            x + BORDER_W,
            y + TITLE_H + BORDER_W,
            w - 2 * BORDER_W,
            h - 2 * BORDER_W,
            CLIENT_BG
        );
        return;
    }

    // Single pread for entire window framebuffer
    size_t total_bytes = total_pixels * sizeof(u32);
    size_t read_total = 0;

    while (read_total < total_bytes) {
        ssize_t n = pread(
            window->fb_fd, (u8 *)blit_buf + read_total, total_bytes - read_total, (off_t)read_total
        );

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }

            break;
        }

        if (!n) {
            break;
        }

        read_total += (size_t)n;
    }

    if (read_total < total_bytes) {
        memset((u8 *)blit_buf + read_total, 0, total_bytes - read_total);
    }

    for (u32 row = 0; row < h; row++) {
        i32 dst_y = y + TITLE_H + (i32)row;

        if (dst_y < 0 || dst_y >= (i32)fb_height) {
            continue;
        }

        i32 col_start = 0;
        i32 col_end = (i32)w;

        if (x < 0) {
            col_start = -x;
        }

        if (x + col_end > (i32)fb_width) {
            col_end = (i32)fb_width - x;
        }

        if (col_start < col_end) {
            memcpy(
                &frame[(size_t)dst_y * fb_width + (size_t)(x + col_start)],
                &blit_buf[(size_t)row * w + (size_t)col_start],
                (size_t)(col_end - col_start) * 4
            );
        }
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

    if (blit_buf) {
        free(blit_buf);
        blit_buf = NULL;
        blit_buf_cap = 0;
    }
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

void wm_handle_ws_event(const ws_event_t *event) {
    if (!event) {
        return;
    }

    wm_window_t *window = wm_window_by_id(event->id);

    if (event->type == WS_EVT_WINDOW_CLOSED) {
        if (!window) {
            return;
        }

        _cleanup_window(window);

        // swap with last element and pop
        size_t idx = ((u8 *)window - (u8 *)windows->data) / windows->elem_size;
        if (idx + 1 < windows->size) {
            wm_window_t *last = vec_at(windows, windows->size - 1);
            memcpy(window, last, sizeof(wm_window_t));
        }

        windows->size--;
        return;
    }

    if (event->type != WS_EVT_WINDOW_NEW) {
        return;
    }

    // if a window with this id already exists, clean it first
    if (window) {
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
        .title = {0},
    };

    strncpy(new_window.title, event->title, sizeof(new_window.title) - 1);
    vec_push(windows, &new_window);
}

void wm_render_frame(u32 *frame, u32 fb_width, u32 fb_height) {
    if (!frame) {
        return;
    }

    if (!wm_background_draw(frame, fb_width, fb_height)) {
        draw_rect(frame, fb_width, fb_height, 0, 0, fb_width, fb_height, BG_COLOR);
    }

    size_t count = windows->size;
    if (!count) {
        return;
    }

    if (count > WS_MAX_WINDOWS) {
        count = WS_MAX_WINDOWS;
    }

    wm_window_t *order[WS_MAX_WINDOWS];

    for (size_t i = 0; i < count; i++) {
        order[i] = vec_at(windows, i);
    }

    _sort_by_z(order, count);

    for (size_t i = 0; i < count; i++) {
        _blit_window(frame, fb_width, fb_height, order[i]);
    }
}

void wm_cleanup_all_windows(void) {
    for (size_t i = 0; i < windows->size; i++) {
        wm_window_t *window = vec_at(windows, i);
        _cleanup_window(window);
    }

    vec_clear(windows);
}
