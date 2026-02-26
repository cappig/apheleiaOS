#pragma once

#include <base/types.h>
#include <stddef.h>

typedef struct {
    u8 pending[4];
    size_t pending_len;
} term_utf8_state_t;

typedef struct {
    void (*on_codepoint)(void *ctx, u32 codepoint);
    void (*on_invalid)(void *ctx);
} term_utf8_callbacks_t;

void term_utf8_reset(term_utf8_state_t *state);
void term_utf8_feed(
    term_utf8_state_t *state,
    u8 byte,
    const term_utf8_callbacks_t *callbacks,
    void *ctx
);
void term_utf8_flush_invalid(
    term_utf8_state_t *state,
    const term_utf8_callbacks_t *callbacks,
    void *ctx
);
