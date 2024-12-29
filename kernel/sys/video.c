#include "video.h"

#include <base/addr.h>
#include <gfx/state.h>
#include <gfx/vga.h>
#include <string.h>

graphics_state video = {0};


void video_init(graphics_state* state) {
    memcpy(&video, state, sizeof(graphics_state));

    dump_gfx_info(&video);
}
