#pragma once

#include <base/types.h>
#include <data/vector.h>
#include <gui/fb.h>
#include <gui/ws.h>
#include <stdbool.h>
#include <ui.h>

#define TITLE_H        18
#define BORDER_W       1
#define CLOSE_BTN_SIZE 12

#define WM_MAX_FB_W   2048
#define WM_MAX_FB_H   1536
#define WM_MAX_FB_PIX (WM_MAX_FB_W * WM_MAX_FB_H)

typedef struct {
    i32 x;
    i32 y;
    i32 width;
    i32 height;
} wm_rect_t;

typedef struct {
    u32 background;
    u32 border;
    u32 title;
    u32 title_focus;
    u32 client_bg;
    u32 title_text;
    u32 close_bg;
    u32 close_fg;
} wm_palette_t;

typedef struct {
    u32 id;
    i32 x;
    i32 y;
    u32 width;
    u32 height;
    u32 fb_width;
    u32 fb_height;
    u32 z;
    bool focused;
    int fb_fd;
    pixel_t *surface;
    size_t surface_pixels;
    size_t surface_capacity;
    u32 surface_width;
    u32 surface_height;
    bool surface_dirty;
    u32 dirty_x;
    u32 dirty_y;
    u32 dirty_width;
    u32 dirty_height;
    char title[WS_TITLE_MAX];
} wm_window_t;

void wm_init(void);
void wm_destroy(void);
void wm_palette_set(const wm_palette_t *palette);
const wm_palette_t *wm_palette_get(void);

wm_window_t *wm_window_by_id(u32 id);
wm_window_t *wm_top_window_at(i32 px, i32 py);
wm_window_t *wm_top_window(void);
bool wm_point_in_title(const wm_window_t *window, i32 px, i32 py);
bool wm_point_in_close(const wm_window_t *window, i32 px, i32 py);
bool wm_window_bounds_rect(const wm_window_t *window, wm_rect_t *rect);
void wm_collect_raise_damage(const wm_window_t *window, u32 old_z, wm_rect_t *damage);

void wm_set_focus(ui_t *ui, wm_window_t *window, u32 *z_counter);
bool wm_handle_ws_event(const ws_event_t *event, wm_rect_t *damage);
void wm_render_damage(pixel_t *frame, u32 fb_width, u32 fb_height, const wm_rect_t *damage);
void wm_render_frame(pixel_t *frame, u32 fb_width, u32 fb_height);
void wm_cleanup_all_windows(void);
