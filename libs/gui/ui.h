#pragma once

#include <base/types.h>
#include <gui/input.h>
#include <gui/ws.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    int ctl_fd;
    int mgr_fd;
    int input_fd;
} ui_t;

typedef struct {
    ui_t *ui;
    ui_t ui_local;
    bool ui_owned;
    int fb_fd;
    int ev_fd;
    u32 id;
    u32 width;
    u32 height;
    u32 stride;
    u32 *pixels;
    size_t pixels_count;
} window_t;

enum ui_open_flags {
    UI_OPEN_INPUT = 1U << 0,
};

int ui_open(ui_t *ui, u32 flags);
void ui_close(ui_t *ui);

ssize_t ui_input(ui_t *ui, input_event_t *events, size_t count);

int ui_mgr_claim(ui_t *ui);
int ui_mgr_release(ui_t *ui);
ssize_t ui_mgr_events(ui_t *ui, ws_event_t *events, size_t count);
int ui_mgr_focus(ui_t *ui, u32 id);
int ui_mgr_move(ui_t *ui, u32 id, i32 x, i32 y);
int ui_mgr_raise(ui_t *ui, u32 id, u32 z);
int ui_mgr_close(ui_t *ui, u32 id);
int ui_mgr_send(ui_t *ui, u32 id, const input_event_t *event);

int window_alloc(ui_t *ui, window_t *window, u32 width, u32 height, const char *title);
int window_from_env(ui_t *ui, window_t *window);
int window_free(window_t *window);
void window_close(window_t *window);
ssize_t window_blit(window_t *window, const void *pixels, size_t len, size_t offset);
ssize_t window_events(window_t *window, ws_input_event_t *events, size_t count);

// Simple application-facing helpers (open -> draw to window_buffer -> window_flush -> loop events).
int window_init(window_t *window, u32 width, u32 height, const char *title);
void window_deinit(window_t *window);
u32 *window_buffer(window_t *window);
int window_flush(window_t *window);
int window_flush_rect(window_t *window, u32 x, u32 y, u32 width, u32 height);
int window_wait_event(window_t *window, ws_input_event_t *event, int timeout_ms);
