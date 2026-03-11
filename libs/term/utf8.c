#include "utf8.h"

#include <base/utf8.h>
#include <string.h>

static void term_utf8_emit_codepoint(
    const term_utf8_callbacks_t *callbacks,
    void *ctx,
    u32 codepoint
) {
    if (callbacks && callbacks->on_codepoint) {
        callbacks->on_codepoint(ctx, codepoint);
    }
}

static void term_utf8_emit_invalid(
    const term_utf8_callbacks_t *callbacks,
    void *ctx
) {
    if (callbacks && callbacks->on_invalid) {
        callbacks->on_invalid(ctx);
    }
}

void term_utf8_reset(term_utf8_state_t *state) {
    if (!state) {
        return;
    }

    memset(state, 0, sizeof(*state));
}

void term_utf8_flush_invalid(
    term_utf8_state_t *state,
    const term_utf8_callbacks_t *callbacks,
    void *ctx
) {
    if (!state || !state->pending_len) {
        return;
    }

    state->pending_len = 0;
    term_utf8_emit_invalid(callbacks, ctx);
}

void term_utf8_feed(
    term_utf8_state_t *state,
    u8 byte,
    const term_utf8_callbacks_t *callbacks,
    void *ctx
) {
    if (!state) {
        return;
    }

    if (!state->pending_len && byte < 0x80) {
        term_utf8_emit_codepoint(callbacks, ctx, byte);
        return;
    }

    if (state->pending_len >= sizeof(state->pending)) {
        state->pending_len = 0;
        term_utf8_emit_invalid(callbacks, ctx);
    }

    state->pending[state->pending_len++] = byte;

    size_t needed = utf8_sequence_len(state->pending[0]);

    if (!needed) {
        state->pending_len = 0;
        term_utf8_emit_invalid(callbacks, ctx);
        return;
    }

    if (state->pending_len < needed) {
        if (state->pending_len > 1 && (byte & 0xc0) != 0x80) {
            state->pending_len = 0;
            term_utf8_emit_invalid(callbacks, ctx);

            if (byte < 0x80) {
                term_utf8_emit_codepoint(callbacks, ctx, byte);
            } else if (utf8_sequence_len(byte) > 1) {
                state->pending[state->pending_len++] = byte;
            } else {
                term_utf8_emit_invalid(callbacks, ctx);
            }
        }
        return;
    }

    u32 codepoint = 0;
    size_t decoded = utf8_decode(state->pending, needed, &codepoint);

    state->pending_len = 0;

    if (!decoded) {
        term_utf8_emit_invalid(callbacks, ctx);
        return;
    }

    term_utf8_emit_codepoint(callbacks, ctx, codepoint);
}
