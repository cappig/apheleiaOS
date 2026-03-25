#pragma once

#include <base/types.h>
#include <gui/fb.h>
#include <gui/input.h>
#include <gui/ws.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    int ctl_fd;
    int mgr_fd;
    int keyboard_fd;
    int mouse_fd;
    u32 key_modifiers;
    u32 mouse_buttons;
    bool input_round_robin;
    u8 pending_head;
    u8 pending_count;
    input_event_t pending_events[3];
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
    pixel_t *pixels;
    size_t pixels_count;
    size_t pixels_capacity;
    framebuffer_t framebuffer;
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
int ui_mgr_resize(ui_t *ui, u32 id, u32 width, u32 height);
int ui_mgr_raise(ui_t *ui, u32 id, u32 z);
int ui_mgr_close(ui_t *ui, u32 id);
int ui_mgr_send(ui_t *ui, u32 id, const input_event_t *event);

int window_set_title(window_t *window, const char *title);
void window_close(window_t *window);
ssize_t window_events(window_t *window, ws_input_event_t *events, size_t count);

int window_init(window_t *window, u32 width, u32 height, const char *title);
void window_deinit(window_t *window);
framebuffer_t *window_buffer(window_t *window);
int window_flush(window_t *window);
int window_flush_rect(window_t *window, u32 x, u32 y, u32 width, u32 height);
int window_wait_event(
    window_t *window,
    ws_input_event_t *event,
    int timeout_ms
);
