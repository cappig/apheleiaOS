#pragma once

#include <base/types.h>
#include <data/vector.h>
#include <gui/ws.h>
#include <stdbool.h>
#include <ui.h>

#define TITLE_H        18
#define BORDER_W       1
#define CLOSE_BTN_SIZE 12

#define BG_COLOR      0x00202020U
#define WM_MAX_FB_W   2048
#define WM_MAX_FB_H   1536
#define WM_MAX_FB_PIX (WM_MAX_FB_W * WM_MAX_FB_H)

typedef struct {
    u32 id;
    i32 x;
    i32 y;
    u32 width;
    u32 height;
    u32 z;
    bool focused;
    int fb_fd;
    char title[WS_TITLE_MAX];
} wm_window_t;

void wm_init(void);
void wm_destroy(void);

wm_window_t *wm_window_by_id(u32 id);
wm_window_t *wm_top_window_at(i32 px, i32 py);
wm_window_t *wm_top_window(void);
bool wm_point_in_title(const wm_window_t *window, i32 px, i32 py);
bool wm_point_in_close(const wm_window_t *window, i32 px, i32 py);

void wm_set_focus(ui_t *ui, wm_window_t *window, u32 *z_counter);
void wm_handle_ws_event(const ws_event_t *event);
void wm_render_frame(u32 *frame, u32 fb_width, u32 fb_height);
void wm_cleanup_all_windows(void);
