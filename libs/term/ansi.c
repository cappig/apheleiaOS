#include "ansi.h"

#include <string.h>

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
