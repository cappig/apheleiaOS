#pragma once

#include <base/types.h>
#include <gui/fb.h>
#include <signal.h>
#include <stddef.h>
#include <ui.h>

void wm_loop(
    ui_t *ui,
    int fb_fd,
    const fb_info_t *fb_info,
    u32 *frame_store,
    size_t frame_bytes,
    volatile sig_atomic_t *exit_requested
);
