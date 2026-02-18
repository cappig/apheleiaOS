#pragma once

#include <base/types.h>
#include <sys/types.h>

#define WS_MAX_WINDOWS 16
#define WS_TITLE_MAX   64

enum ws_op {
    WS_OP_CLAIM_MANAGER = 1,
    WS_OP_RELEASE_MANAGER = 2,
    WS_OP_SET_FOCUS = 3,
    WS_OP_SET_POS = 4,
    WS_OP_SET_Z = 5,
    WS_OP_SEND_INPUT = 6,
    WS_OP_CLOSE = 7,
    WS_OP_ALLOC = 8,
    WS_OP_FREE = 9,
    WS_OP_QUERY = 10,
    WS_OP_CLEAR_DIRTY = 11,
};

enum ws_event_type {
    WS_EVT_WINDOW_NEW = 1,
    WS_EVT_WINDOW_CLOSED = 2,
};

enum ws_window_flags {
    WS_WINDOW_MAPPED = 1 << 0,
    WS_WINDOW_FOCUSED = 1 << 1,
};

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
} ws_input_event_t;

typedef struct ws_req {
    u32 op;
    u32 id;
    i32 x;
    i32 y;
    u32 width;
    u32 height;
    u32 flags;
    pid_t pid;
    char title[WS_TITLE_MAX];
    ws_input_event_t input;
} ws_req_t;

typedef struct ws_resp {
    i32 status;
    u32 id;
    i32 x;
    i32 y;
    u32 width;
    u32 height;
    u32 stride;
    u32 flags;
} ws_resp_t;

typedef struct ws_event {
    u32 type;
    u32 id;
    pid_t owner_pid;
    i32 x;
    i32 y;
    u32 width;
    u32 height;
    char title[WS_TITLE_MAX];
} ws_event_t;
