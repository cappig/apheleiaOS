#include "state.h"

#include <log/log.h>

static char* _stringify_mode(graphics_mode mode) {
    switch (mode) {
    case GFX_VESA:
        return "VESA graphics";
    case GFX_VGA:
        return "VGA text";
    case GFX_NONE:
        return "headless";
    default:
        return "INVALID";
    }
}


void dump_gfx_info(graphics_state* gfx) {
    log_info(
        "Running in %s mode with a resolution of %dx%d:%d",
        _stringify_mode(gfx->mode),
        gfx->width,
        gfx->height,
        gfx->depth * 8
    );
}
