#include "config.h"

#include <lib/boot.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tty.h"
#include "x86/boot/bios/disk.h"

static void _config_cmp(char* key, char* value, void* data) {
    kernel_args_t* args = data;

    if (!strcasecmp(key, "debug")) {
        if (!strcasecmp(value, "all"))
            args->debug = DEBUG_ALL;
        else if (!strcasecmp(value, "minimal"))
            args->debug = DEBUG_MINIMAL;
        else if (!strcasecmp(value, "none"))
            args->debug = DEBUG_NONE;
    }

    else if (!strcasecmp(key, "video")) {
        if (!strcasecmp(value, "graphics"))
            args->video = VIDEO_GRAPHICS;
        else if (!strcasecmp(value, "text"))
            args->video = VIDEO_TEXT;
        else if (!strcasecmp(value, "none"))
            args->video = VIDEO_NONE;
    }

    else if (!strcasecmp(key, "vesa.width")) {
        i32 width = atoll(value);
        if (width < 0)
            return;

        args->vesa_width = width;
    }

    else if (!strcasecmp(key, "vesa.height")) {
        i32 height = atoll(value);
        if (height < 0)
            return;

        args->vesa_height = height;
    }

    else if (!strcasecmp(key, "vesa.bpp")) {
        i32 bpp = atoll(value);
        if (bpp < 32)
            return;

        args->vesa_bpp = bpp;
    }
}


void parse_config(kernel_args_t* args) {
    // memset(args, 0, sizeof(kernel_args_t));

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

    parse_cfg(config, _config_cmp, args);

    free(config);
}
