#pragma once

#include <base/types.h>
#include <stdbool.h>
#include <stddef.h>

#define ANSI_MAX_PARAMS 8

typedef struct {
    bool esc;
    bool csi;
    bool csi_private;
    int params[ANSI_MAX_PARAMS];
    size_t param_count;
    int current;
} ansi_parser_t;

typedef struct {
    u8 fg_idx;
    u8 bg_idx;
    bool bright;
    bool reverse;
} ansi_color_state_t;

typedef struct {
    void (*on_print)(void *ctx, u8 ch);
    void (*on_control)(void *ctx, u8 ch);
    void (*on_csi)(
        void *ctx,
        char final,
        const int *params,
        size_t count,
        bool private_mode
    );
    void (*on_escape)(void *ctx, u8 ch);
} ansi_callbacks_t;

typedef struct {
    size_t *cursor_x;
    size_t *cursor_y;
    size_t *saved_x;
    size_t *saved_y;
    bool *saved_valid;
    bool *cursor_visible;
    size_t cols;
    size_t rows;
    ansi_color_state_t *color;
    void (*clear_screen)(void *ctx, int mode);
    void (*clear_line)(void *ctx, int mode);
    void (*cursor_show)(void *ctx);
    void (*cursor_hide)(void *ctx);
    void *ctx;
} ansi_csi_state_t;

void ansi_parser_reset(ansi_parser_t *parser);
void ansi_parser_init(ansi_parser_t *parser);
void ansi_parser_feed(
    ansi_parser_t *parser,
    u8 ch,
    const ansi_callbacks_t *callbacks,
    void *ctx
);

int ansi_param(const int *params, size_t count, size_t index, int fallback);
void ansi_csi_dispatch_state(
    char op,
    const int *params,
    size_t count,
    bool private_mode,
    ansi_csi_state_t *state
);

u32 ansi_color_rgb(u8 idx);
void ansi_color_reset(ansi_color_state_t *state);
void ansi_color_apply_sgr(ansi_color_state_t *state, int code);
