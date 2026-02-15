#include <draw.h>
#include <errno.h>
#include <fcntl.h>
#include <psf.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "wm.h"

#define BORDER_COLOR   0x00c0c0c0U
#define TITLE_COLOR    0x00306090U
#define TITLE_FOCUS    0x0060a0d0U
#define CLIENT_BG      0x00101010U
#define TITLE_TEXT     0x00f0f0f0U
#define CLOSE_BG       0x00b04040U
#define CLOSE_FG       0x00ffffffU
#define FONT_BUF_SIZE  (256 * 1024)
#define MAX_TITLE_CHARS 48

static wm_window_t windows[WS_MAX_WINDOWS];
static u32 row_store[WM_MAX_FB_W];
static u8 font_buf[FONT_BUF_SIZE];
static psf_font_t title_font = {0};

static int _open_ws_fb(u32 id) {
    char path[64];
    snprintf(path, sizeof(path), "/dev/ws/%u/fb", id);
    return open(path, O_RDWR, 0);
}

static bool _load_font_file(const char* path) {
    return psf_load_file(path, font_buf, sizeof(font_buf), &title_font);
}

static void _init_font(void) {
    if (_load_font_file("/boot/font.psf"))
        return;

    if (_load_font_file("/etc/font.psf"))
        return;

    memset(&title_font, 0, sizeof(title_font));
}

static void _draw_pixel(u32* frame, u32 fb_width, u32 fb_height, i32 x, i32 y, u32 color) {
    if (!frame)
        return;

    if (x < 0 || y < 0 || x >= (i32)fb_width || y >= (i32)fb_height)
        return;

    frame[(size_t)y * fb_width + (size_t)x] = color;
}

static void _draw_text(u32* frame, u32 fb_width, u32 fb_height, i32 x, i32 y, const char* text, u32 color) {
    if (!title_font.glyphs || !text || !text[0])
        return;

    i32 pen_x = x;

    for (const char* p = text; *p && (p - text) < MAX_TITLE_CHARS; p++) {
        u32 idx = (u8)(*p);

        if (idx >= title_font.glyph_count)
            idx = '?';

        const u8* glyph = title_font.glyphs + (size_t)idx * title_font.glyph_size;

        for (u32 gy = 0; gy < title_font.height; gy++) {
            const u8* row_ptr = glyph + gy * title_font.row_bytes;

            for (u32 gx = 0; gx < title_font.width; gx++) {
                u8 bits = row_ptr[gx / 8];
                u8 mask = (u8)(0x80 >> (gx & 7));

                if (bits & mask)
                    _draw_pixel(frame, fb_width, fb_height, pen_x + (i32)gx, y + (i32)gy, color);
            }
        }

        pen_x += (i32)title_font.width;
    }
}

static void _draw_close_button(u32* frame, u32 fb_width, u32 fb_height, const wm_window_t* window, bool focused) {
    if (!window)
        return;

    i32 bx = window->x + (i32)window->width - CLOSE_BTN_SIZE - 3;
    i32 by = window->y + 3;
    u32 bg = focused ? CLOSE_BG : TITLE_COLOR;

    draw_fill_rect(frame, fb_width, fb_height, bx, by, CLOSE_BTN_SIZE, CLOSE_BTN_SIZE, bg);

    for (i32 i = 2; i < CLOSE_BTN_SIZE - 2; i++) {
        _draw_pixel(frame, fb_width, fb_height, bx + i, by + i, CLOSE_FG);
        _draw_pixel(frame, fb_width, fb_height, bx + i, by + (i32)CLOSE_BTN_SIZE - 1 - i, CLOSE_FG);
    }
}

static void _cleanup_window(wm_window_t* window) {
    if (!window)
        return;

    if (window->fb_fd >= 0)
        close(window->fb_fd);

    memset(window, 0, sizeof(*window));
    window->fb_fd = -1;
}

static bool _point_in_rect(i32 px, i32 py, i32 x, i32 y, i32 w, i32 h) {
    return px >= x && py >= y && px < x + w && py < y + h;
}

static bool _point_in_window(const wm_window_t* window, i32 px, i32 py) {
    if (!window || !window->used)
        return false;

    return _point_in_rect(px, py, window->x, window->y, (i32)window->width, (i32)window->height + TITLE_H);
}

static void _sort_by_z(u32* ids, size_t count) {
    if (!ids || count < 2)
        return;

    for (size_t i = 1; i < count; i++) {
        u32 value = ids[i];
        size_t j = i;

        while (j > 0 && windows[ids[j - 1]].z > windows[value].z) {
            ids[j] = ids[j - 1];
            j--;
        }

        ids[j] = value;
    }
}

static void _blit_window(u32* frame, u32 fb_width, u32 fb_height, wm_window_t* window) {
    if (!frame || !window || !window->used)
        return;

    i32 x = window->x;
    i32 y = window->y;
    u32 w = window->width;
    u32 h = window->height;

    draw_fill_rect(frame, fb_width, fb_height, x, y, w, TITLE_H, window->focused ? TITLE_FOCUS : TITLE_COLOR);
    draw_fill_rect(frame, fb_width, fb_height, x, y, w, BORDER_W, BORDER_COLOR);
    draw_fill_rect(frame, fb_width, fb_height, x, y + TITLE_H - BORDER_W, w, BORDER_W, BORDER_COLOR);
    draw_fill_rect(frame, fb_width, fb_height, x, y + TITLE_H, BORDER_W, h, BORDER_COLOR);
    draw_fill_rect(frame, fb_width, fb_height, x + (i32)w - BORDER_W, y + TITLE_H, BORDER_W, h, BORDER_COLOR);
    draw_fill_rect(
        frame, fb_width, fb_height,
        x + BORDER_W, y + TITLE_H + BORDER_W,
        w - 2 * BORDER_W, h - 2 * BORDER_W, CLIENT_BG
    );

    _draw_close_button(frame, fb_width, fb_height, window, window->focused);

    if (window->title[0])
        _draw_text(frame, fb_width, fb_height, x + 6, y + 1, window->title, TITLE_TEXT);

    if (window->fb_fd < 0)
        return;

    u32 copy_cols = w;
    if (copy_cols > WM_MAX_FB_W)
        copy_cols = WM_MAX_FB_W;

    for (u32 row = 0; row < h; row++) {
        off_t row_off = (off_t)((size_t)row * (size_t)w * 4);
        size_t row_bytes = copy_cols * 4;

        ssize_t n = pread(window->fb_fd, row_store, row_bytes, row_off);

        if (n < 0) {
            if (errno == EINTR)
                continue;

            break;
        }

        if ((size_t)n != row_bytes)
            continue;

        i32 dst_y = y + TITLE_H + (i32)row;

        if (dst_y < 0 || dst_y >= (i32)fb_height)
            continue;

        for (u32 col = 0; col < copy_cols; col++) {
            i32 dst_x = x + (i32)col;

            if (dst_x < 0 || dst_x >= (i32)fb_width)
                continue;

            frame[(size_t)dst_y * fb_width + (size_t)dst_x] = row_store[col];
        }
    }
}

void wm_init(void) {
    _init_font();
    for (u32 i = 0; i < WS_MAX_WINDOWS; i++)
        windows[i].fb_fd = -1;
}

wm_window_t* wm_window_by_id(u32 id) {
    if (id >= WS_MAX_WINDOWS)
        return NULL;

    return &windows[id];
}

bool wm_point_in_title(const wm_window_t* window, i32 px, i32 py) {
    if (!window || !window->used)
        return false;

    return _point_in_rect(px, py, window->x, window->y, (i32)window->width, TITLE_H);
}

bool wm_point_in_close(const wm_window_t* window, i32 px, i32 py) {
    if (!window || !window->used)
        return false;

    i32 bx = window->x + (i32)window->width - CLOSE_BTN_SIZE - 3;
    i32 by = window->y + 3;

    return _point_in_rect(px, py, bx, by, CLOSE_BTN_SIZE, CLOSE_BTN_SIZE);
}

wm_window_t* wm_top_window_at(i32 px, i32 py) {
    wm_window_t* top = NULL;

    for (u32 i = 0; i < WS_MAX_WINDOWS; i++) {
        wm_window_t* window = &windows[i];

        if (!_point_in_window(window, px, py))
            continue;

        if (!top || window->z >= top->z)
            top = window;
    }

    return top;
}

wm_window_t* wm_top_window(void) {
    wm_window_t* top = NULL;

    for (u32 i = 0; i < WS_MAX_WINDOWS; i++) {
        wm_window_t* window = &windows[i];

        if (!window->used)
            continue;

        if (!top || window->z >= top->z)
            top = window;
    }

    return top;
}

void wm_set_focus(ui_t* ui, wm_window_t* window, u32* z_counter) {
    if (!window)
        return;

    ui_mgr_focus(ui, window->id);

    if (z_counter) {
        window->z = ++(*z_counter);
        ui_mgr_raise(ui, window->id, window->z);
    }

    for (u32 i = 0; i < WS_MAX_WINDOWS; i++)
        windows[i].focused = false;

    window->focused = true;
}

void wm_handle_ws_event(const ws_event_t* event) {
    if (!event || event->id >= WS_MAX_WINDOWS)
        return;

    wm_window_t* window = wm_window_by_id(event->id);
    if (!window)
        return;

    if (event->type == WS_EVT_WINDOW_CLOSED) {
        _cleanup_window(window);
        return;
    }

    if (event->type != WS_EVT_WINDOW_NEW)
        return;

    _cleanup_window(window);

    window->used = true;
    window->id = event->id;
    window->x = event->x;
    window->y = event->y;
    window->width = event->width;
    window->height = event->height;
    window->z = event->id;
    window->focused = false;
    strncpy(window->title, event->title, sizeof(window->title) - 1);
    window->title[sizeof(window->title) - 1] = '\0';
    window->fb_fd = _open_ws_fb(window->id);
}

void wm_render_frame(u32* frame, u32 fb_width, u32 fb_height) {
    if (!frame)
        return;

    draw_fill_rect(frame, fb_width, fb_height, 0, 0, fb_width, fb_height, BG_COLOR);

    u32 order[WS_MAX_WINDOWS];
    size_t active = 0;

    for (u32 i = 0; i < WS_MAX_WINDOWS; i++) {
        if (!windows[i].used)
            continue;

        order[active++] = i;
    }

    _sort_by_z(order, active);

    for (size_t i = 0; i < active; i++)
        _blit_window(frame, fb_width, fb_height, &windows[order[i]]);
}

void wm_cleanup_all_windows(void) {
    for (u32 i = 0; i < WS_MAX_WINDOWS; i++)
        _cleanup_window(&windows[i]);
}
