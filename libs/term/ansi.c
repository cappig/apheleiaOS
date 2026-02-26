#include "ansi.h"

#include "cursor.h"

#include <string.h>

static const u32 ansi_rgb[16] = {
    0x000000,
    0x800000,
    0x008000,
    0x808000,
    0x000080,
    0x800080,
    0x008080,
    0xc0c0c0,
    0x808080,
    0xff0000,
    0x00ff00,
    0xffff00,
    0x0000ff,
    0xff00ff,
    0x00ffff,
    0xffffff,
};

static void ansi_color_set_render_fg_idx(ansi_color_state_t *state, u8 idx) {
    if (!state) {
        return;
    }

    if (state->reverse) {
        state->bg_idx = (u8)(idx & 0x0f);
        return;
    }

    state->fg_idx = (u8)(idx & 0x0f);
}

static void ansi_color_set_render_bg_idx(ansi_color_state_t *state, u8 idx) {
    if (!state) {
        return;
    }

    if (state->reverse) {
        state->fg_idx = (u8)(idx & 0x0f);
        return;
    }

    state->bg_idx = (u8)(idx & 0x0f);
}

static u8 ansi_color_logical_fg_base(const ansi_color_state_t *state) {
    if (!state) {
        return 0x7;
    }

    u8 idx = state->reverse ? state->bg_idx : state->fg_idx;
    return (u8)(idx & 0x7);
}

u32 ansi_color_rgb(u8 idx) {
    return ansi_rgb[idx & 0x0f];
}

void ansi_color_reset(ansi_color_state_t *state) {
    if (!state) {
        return;
    }

    state->fg_idx = 0x7;
    state->bg_idx = 0x0;
    state->bright = false;
    state->reverse = false;
}

static void ansi_color_set_fg(
    ansi_color_state_t *state,
    u8 base,
    bool force_bright
) {
    if (!state) {
        return;
    }

    u8 idx = base & 0x7;
    if (force_bright || state->bright) {
        idx = (u8)(idx + 8);
    }

    ansi_color_set_render_fg_idx(state, idx);
}

static void ansi_color_set_bg(ansi_color_state_t *state, u8 base, bool bright) {
    if (!state) {
        return;
    }

    u8 idx = base & 0x7;
    if (bright) {
        idx = (u8)(idx + 8);
    }

    ansi_color_set_render_bg_idx(state, idx);
}

void ansi_color_apply_sgr(ansi_color_state_t *state, int code) {
    if (!state) {
        return;
    }

    if (!code) {
        ansi_color_reset(state);
        return;
    }

    if (code == 1) {
        state->bright = true;
        ansi_color_set_fg(state, ansi_color_logical_fg_base(state), true);
        return;
    }

    if (code == 2 || code == 22) {
        state->bright = false;
        ansi_color_set_fg(state, ansi_color_logical_fg_base(state), false);
        return;
    }

    if (code == 7) {
        if (!state->reverse) {
            u8 tmp = state->fg_idx;
            state->fg_idx = state->bg_idx;
            state->bg_idx = tmp;
            state->reverse = true;
        }
        return;
    }

    if (code == 27) {
        if (state->reverse) {
            u8 tmp = state->fg_idx;
            state->fg_idx = state->bg_idx;
            state->bg_idx = tmp;
            state->reverse = false;
        }
        return;
    }

    if (code == 39) {
        ansi_color_set_fg(state, 0x7, false);
        return;
    }

    if (code == 49) {
        ansi_color_set_bg(state, 0x0, false);
        return;
    }

    if (code >= 30 && code <= 37) {
        ansi_color_set_fg(state, (u8)(code - 30), false);
        return;
    }

    if (code >= 90 && code <= 97) {
        ansi_color_set_fg(state, (u8)(code - 90), true);
        return;
    }

    if (code >= 40 && code <= 47) {
        ansi_color_set_bg(state, (u8)(code - 40), false);
        return;
    }

    if (code >= 100 && code <= 107) {
        ansi_color_set_bg(state, (u8)(code - 100), true);
    }
}

static void ansi_color_apply_sgr_list(
    ansi_color_state_t *state,
    const int *params,
    size_t count
) {
    if (!state) {
        return;
    }

    if (!count) {
        ansi_color_apply_sgr(state, 0);
        return;
    }

    for (size_t i = 0; i < count; i++) {
        ansi_color_apply_sgr(state, params ? params[i] : 0);
    }
}

void ansi_parser_reset(ansi_parser_t *parser) {
    if (!parser) {
        return;
    }

    memset(parser, 0, sizeof(*parser));
    parser->current = -1;
}

void ansi_parser_init(ansi_parser_t *parser) {
    ansi_parser_reset(parser);
}

static void ansi_reset_escape(ansi_parser_t *parser) {
    parser->esc = false;
    parser->csi = false;
    parser->csi_private = false;
    parser->param_count = 0;
    parser->current = -1;
}

static void ansi_emit_csi(
    ansi_parser_t *parser,
    u8 ch,
    const ansi_callbacks_t *callbacks,
    void *ctx
) {
    if (parser->current >= 0 && parser->param_count < ANSI_MAX_PARAMS) {
        parser->params[parser->param_count++] = parser->current;
    }

    if (callbacks && callbacks->on_csi) {
        callbacks->on_csi(
            ctx,
            (char)ch,
            parser->params,
            parser->param_count,
            parser->csi_private
        );
    }

    ansi_reset_escape(parser);
}

void ansi_parser_feed(
    ansi_parser_t *parser,
    u8 ch,
    const ansi_callbacks_t *callbacks,
    void *ctx
) {
    if (!parser) {
        return;
    }

    if (parser->esc && parser->csi) {
        if (ch == '?' && !parser->param_count && parser->current < 0) {
            parser->csi_private = true;
            return;
        }

        if (ch >= '0' && ch <= '9') {
            if (parser->current < 0) {
                parser->current = 0;
            }

            parser->current = parser->current * 10 + (int)(ch - '0');
            return;
        }

        if (ch == ';') {
            if (parser->param_count < ANSI_MAX_PARAMS) {
                parser->params[parser->param_count++] =
                    parser->current < 0 ? 0 : parser->current;
            }

            parser->current = -1;
            return;
        }

        if (ch >= '@' && ch <= '~') {
            ansi_emit_csi(parser, ch, callbacks, ctx);
            return;
        }

        ansi_reset_escape(parser);
        return;
    }

    if (parser->esc) {
        if (ch == '[') {
            parser->csi = true;
            parser->csi_private = false;
            parser->param_count = 0;
            parser->current = -1;
            memset(parser->params, 0, sizeof(parser->params));
            return;
        }

        if (callbacks && callbacks->on_escape) {
            callbacks->on_escape(ctx, ch);
        }

        ansi_reset_escape(parser);
        return;
    }

    if (ch == 0x1b) {
        parser->esc = true;
        parser->csi = false;
        return;
    }

    if (ch < 0x20 || ch == 0x7f) {
        if (callbacks && callbacks->on_control) {
            callbacks->on_control(ctx, ch);
        }
        return;
    }

    if (callbacks && callbacks->on_print) {
        callbacks->on_print(ctx, ch);
    }
}

int ansi_param(const int *params, size_t count, size_t index, int fallback) {
    if (!params || index >= count) {
        return fallback;
    }

    return params[index];
}

static int ansi_csi_param_min(
    const int *params,
    size_t count,
    size_t index,
    int fallback,
    int minimum
) {
    int value = ansi_param(params, count, index, fallback);

    if (value < minimum) {
        value = minimum;
    }

    return value;
}

static void ansi_csi_cursor_show(ansi_csi_state_t *state) {
    if (state && state->cursor_show) {
        state->cursor_show(state->ctx);
    }
}

static void ansi_csi_cursor_hide(ansi_csi_state_t *state) {
    if (state && state->cursor_hide) {
        state->cursor_hide(state->ctx);
    }
}

void ansi_csi_dispatch_state(
    char op,
    const int *params,
    size_t count,
    bool private_mode,
    ansi_csi_state_t *state
) {
    if (!state) {
        return;
    }

    switch (op) {
    case 'A':
        if (state->cursor_x && state->cursor_y) {
            int move = ansi_csi_param_min(params, count, 0, 1, 1);
            term_cursor_move(
                state->cursor_x,
                state->cursor_y,
                state->cols,
                state->rows,
                -move,
                0
            );
            ansi_csi_cursor_show(state);
        }
        break;
    case 'B':
        if (state->cursor_x && state->cursor_y) {
            int move = ansi_csi_param_min(params, count, 0, 1, 1);
            term_cursor_move(
                state->cursor_x,
                state->cursor_y,
                state->cols,
                state->rows,
                move,
                0
            );
            ansi_csi_cursor_show(state);
        }
        break;
    case 'C':
        if (state->cursor_x && state->cursor_y) {
            int move = ansi_csi_param_min(params, count, 0, 1, 1);
            term_cursor_move(
                state->cursor_x,
                state->cursor_y,
                state->cols,
                state->rows,
                0,
                move
            );
            ansi_csi_cursor_show(state);
        }
        break;
    case 'D':
        if (state->cursor_x && state->cursor_y) {
            int move = ansi_csi_param_min(params, count, 0, 1, 1);
            term_cursor_move(
                state->cursor_x,
                state->cursor_y,
                state->cols,
                state->rows,
                0,
                -move
            );
            ansi_csi_cursor_show(state);
        }
        break;
    case 'G':
        if (state->cursor_x) {
            int col = ansi_csi_param_min(params, count, 0, 1, 1);
            term_cursor_set_col(state->cursor_x, state->cols, col);
            ansi_csi_cursor_show(state);
        }
        break;
    case 'H':
    case 'f':
        if (state->cursor_x && state->cursor_y) {
            int row = ansi_csi_param_min(params, count, 0, 1, 1);
            int col = ansi_csi_param_min(params, count, 1, 1, 1);
            term_cursor_set_pos(
                state->cursor_x,
                state->cursor_y,
                state->cols,
                state->rows,
                row,
                col
            );
            ansi_csi_cursor_show(state);
        }
        break;
    case 'J':
        if (state->clear_screen) {
            state->clear_screen(
                state->ctx, ansi_param(params, count, 0, 0)
            );
        }
        break;
    case 'K':
        if (state->clear_line) {
            state->clear_line(state->ctx, ansi_param(params, count, 0, 0));
        }
        break;
    case 's':
        if (state->cursor_x && state->cursor_y && state->saved_x && state->saved_y) {
            term_cursor_save(
                state->cursor_x,
                state->cursor_y,
                state->saved_x,
                state->saved_y,
                state->saved_valid
            );
        }
        break;
    case 'u':
        if (
            state->cursor_x &&
            state->cursor_y &&
            state->saved_x &&
            state->saved_y &&
            term_cursor_restore(
                state->cursor_x,
                state->cursor_y,
                state->cols,
                state->rows,
                state->saved_x,
                state->saved_y,
                state->saved_valid
            )
        ) {
            ansi_csi_cursor_show(state);
        }
        break;
    case 'h':
        if (
            private_mode &&
            ansi_param(params, count, 0, 0) == 25
        ) {
            if (state->cursor_visible) {
                *state->cursor_visible = true;
            }

            ansi_csi_cursor_show(state);
        }
        break;
    case 'l':
        if (
            private_mode &&
            ansi_param(params, count, 0, 0) == 25
        ) {
            if (state->cursor_visible) {
                *state->cursor_visible = false;
            }

            ansi_csi_cursor_hide(state);
        }
        break;
    case 'm':
        ansi_color_apply_sgr_list(state->color, params, count);
        break;
    default:
        break;
    }
}
