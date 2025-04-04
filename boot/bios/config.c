#include "config.h"

#include <boot/proto.h>
#include <stdlib.h>
#include <string.h>

static void _config_cmp(char* key, char* value, void* data) {
    boot_args* args = data;

    if (!strcasecmp(key, "DEBUG")) {
        if (!strcasecmp(value, "all"))
            args->debug = DEBUG_ALL;
        else if (!strcasecmp(value, "minimal"))
            args->debug = DEBUG_MINIMAL;
        else if (!strcasecmp(value, "none"))
            args->debug = DEBUG_NONE;
    }

    else if (!strcasecmp(key, "GFX_MODE")) {
        if (!strcasecmp(value, "vesa"))
            args->gfx_mode = GFX_VESA;
        else if (!strcasecmp(value, "vga"))
            args->gfx_mode = GFX_VGA;
        else if (!strcasecmp(value, "none"))
            args->gfx_mode = GFX_NONE;
    }

    else if (!strcasecmp(key, "VESA_WIDTH")) {
        i32 width = atoll(value);
        if (width < 0)
            return;

        args->vesa_width = width;
    }

    else if (!strcasecmp(key, "VESA_HEIGHT")) {
        i32 height = atoll(value);
        if (height < 0)
            return;

        args->vesa_height = height;
    }

    else if (!strcasecmp(key, "VESA_BPP")) {
        i32 bpp = atoll(value);
        if (bpp < 32)
            return;

        args->vesa_bpp = bpp;
    }
}


void parse_config(file_handle* file, boot_args* args) {
    memset(args, 0, sizeof(boot_args));

    args->debug = BOOT_DEFAULT_DEBUG;
    args->gfx_mode = BOOT_DEFAULT_GFX_MODE;
    args->vesa_width = BOOT_DEFAULT_VESA_WIDTH;
    args->vesa_height = BOOT_DEFAULT_VESA_HEIGHT;
    args->vesa_bpp = BOOT_DEFAULT_VESA_BPP;

    if (!file->size)
        return;

    parse_cfg(file->addr, _config_cmp, args);
}
