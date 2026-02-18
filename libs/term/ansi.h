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
    void (*on_print)(void *ctx, u8 ch);
    void (*on_control)(void *ctx, u8 ch);
    void (*on_csi)(void *ctx, char final, const int *params, size_t count, bool private_mode);
    void (*on_escape)(void *ctx, u8 ch);
} ansi_callbacks_t;

void ansi_parser_reset(ansi_parser_t *parser);
void ansi_parser_init(ansi_parser_t *parser);
void ansi_parser_feed(ansi_parser_t *parser, u8 ch, const ansi_callbacks_t *callbacks, void *ctx);

int ansi_param(const int *params, size_t count, size_t index, int fallback);
