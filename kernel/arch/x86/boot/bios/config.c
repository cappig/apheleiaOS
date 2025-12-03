#include "config.h"

#include <lib/boot.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "disk.h"
#include "tty.h"

static void handle_debug(char* value, void* data) {
    kernel_args_t* args = data;

    if (!strcasecmp(value, "all"))
        args->debug = DEBUG_ALL;
    else if (!strcasecmp(value, "minimal"))
        args->debug = DEBUG_MINIMAL;
    else if (!strcasecmp(value, "none"))
        args->debug = DEBUG_NONE;
}

static void handle_video(char* value, void* data) {
    kernel_args_t* args = data;

    if (!strcasecmp(value, "graphics"))
        args->video = VIDEO_GRAPHICS;
    else if (!strcasecmp(value, "text"))
        args->video = VIDEO_TEXT;
    else if (!strcasecmp(value, "none"))
        args->video = VIDEO_NONE;
}

static void handle_vesa_width(char* value, void* data) {
    kernel_args_t* args = data;

    i32 width = atoll(value);
    if (width < 0)
        return;

    args->vesa_width = width;
}

static void handle_vesa_height(char* value, void* data) {
    kernel_args_t* args = data;

    i32 height = atoll(value);
    if (height < 0)
        return;

    args->vesa_height = height;
}

static void handle_vesa_bpp(char* value, void* data) {
    kernel_args_t* args = data;
    args->vesa_bpp = atol(value);
}

static void handle_console(char* value, void* data) {
    kernel_args_t* args = data;
    strncpy(args->console, value, 128);
}


static const cfg_entry_t cfg_table[] = {
    {"debug", handle_debug},
    {"video", handle_video},
    {"graphics.width", handle_vesa_width},
    {"graphics.height", handle_vesa_height},
    {"graphics.bpp", handle_vesa_bpp},
    {"console", handle_console},
    {NULL, NULL}
};


void parse_config(kernel_args_t* args) {
    args->debug = BOOT_DEFAULT_DEBUG;
    args->video = BOOT_DEFAULT_VIDEO;
    args->vesa_width = BOOT_DEFAULT_VESA_WIDTH;
    args->vesa_height = BOOT_DEFAULT_VESA_HEIGHT;
    args->vesa_bpp = BOOT_DEFAULT_VESA_BPP;

    void* config = read_rootfs("/etc/loader.conf");

    if (!config) {
        puts("/etc/loader.conf not found, using defaults\r\n");
        return;
    }

    parse_cfg(config, cfg_table, args);

    free(config);
}
