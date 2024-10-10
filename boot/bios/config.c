#include "config.h"

#include <stdlib.h>
#include <string.h>

#include "boot/proto.h"

static void _config_cmp(char* key, char* value, void* data) {
    boot_args* args = data;

    // TODO: we may want to hash the string and use a switch statement
    if (!strcasecmp(key, "GFX_MODE")) {
        if (!strcasecmp(value, "vesa")) {
            args->gfx_mode = GFX_VESA;
        } else if (!strcasecmp(value, "vga")) {
            args->gfx_mode = GFX_VGA;
        } else if (!strcasecmp(value, "none")) {
            args->gfx_mode = GFX_NONE;
        }
    }

    else if (!strcasecmp(key, "VESA_WIDTH")) {
        u16 width = atoi(value);
        if (!width)
            return;

        args->vesa_width = width;
    }

    else if (!strcasecmp(key, "VESA_HEIGHT")) {
        u16 height = atoi(value);
        if (!height)
            return;

        args->vesa_height = height;
    }

    else if (!strcasecmp(key, "VESA_BPP")) {
        u16 bpp = atoi(value);
        if (!bpp)
            return;

        args->vesa_bpp = bpp;
    }

    else if (!strcasecmp(key, "CONSOLE_FONT")) {
        strcpy(args->console_font, value);
        args->console_font[BOOT_CONSOLE_FONT_LEN] = '\0';
    }
}


void parse_config(file_handle* file, boot_args* args) {
    memset(args, 0, sizeof(boot_args));

    args->gfx_mode = BOOT_DEFAULT_GFX_MODE;
    args->vesa_width = BOOT_DEFAULT_VESA_WIDTH;
    args->vesa_height = BOOT_DEFAULT_VESA_HEIGHT;
    args->vesa_bpp = BOOT_DEFAULT_VESA_BPP;

    if (!file->size)
        return;

    parse_cfg(file->addr, _config_cmp, args);
}
