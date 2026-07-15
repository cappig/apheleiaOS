#pragma once

#include <base/types.h>
#include <sys/types.h>

#define WS_TITLE_MAX 64

enum ws_event_type {
    WS_EVT_WINDOW_NEW = 1,
    WS_EVT_WINDOW_CLOSED = 2,
    WS_EVT_WINDOW_DIRTY = 3,
    WS_EVT_SCREEN_ACTIVE = 4,
    WS_EVT_WINDOW_TITLE = 5,
};

enum ws_window_flags {
    WS_WINDOW_MAPPED = 1 << 0,
    WS_WINDOW_FOCUSED = 1 << 1,
    WS_WINDOW_FIXED = 1 << 2,
};

typedef struct ws_hints {
    u32 flags;
    u32 min_width;
    u32 min_height;
} ws_hints_t;

typedef struct ws_input_event {
    u64 timestamp_ms;
    u32 type;
    u32 source;
    u32 keycode;
    u32 action;
    u32 buttons;
    u32 modifiers;
    i32 dx;
    i32 dy;
    i32 wheel;
    u32 width;
    u32 height;
    u32 stride;
} ws_input_event_t;

typedef struct ws_event {
    u32 type;
    u32 id;
    pid_t owner_pid;
    i32 x;
    i32 y;
    u32 width;
    u32 height;
    u32 flags;
    u32 min_width;
    u32 min_height;
    char title[WS_TITLE_MAX];
} ws_event_t;

typedef struct ws_cmd {
    u32 id;
    i32 x;
    i32 y;
    u32 width;
    u32 height;
    u32 stride;
    u32 flags;
    u32 min_width;
    u32 min_height;
    pid_t pid;
    char title[WS_TITLE_MAX];
    ws_input_event_t input;
} ws_cmd_t;
