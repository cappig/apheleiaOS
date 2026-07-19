#include "boot_config.h"

#include <parse/cfg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static void copy_value(char *dest, size_t size, const char *value) {
    if (!dest || !size || !value) {
        return;
    }

    strncpy(dest, value, size - 1);
    dest[size - 1] = '\0';
}

static bool parse_bool(const char *value, bool *out) {
    if (!value || !out) {
        return false;
    }

    if (!strcasecmp(value, "1") || !strcasecmp(value, "true")) {
        *out = true;
        return true;
    }

    if (!strcasecmp(value, "0") || !strcasecmp(value, "false")) {
        *out = false;
        return true;
    }

    return false;
}

static bool parse_u16(const char *text, u16 *out) {
    if (!text || !*text || !out) {
        return false;
    }

    char *end = NULL;
    long value = strtol(text, &end, 10);

    if (!end || *end || value < 0 || value > UINT16_MAX) {
        return false;
    }

    *out = (u16)value;
    return true;
}

static void handle_log_level(char *value, void *data) {
    kernel_args_t *args = data;

    if (!strcasecmp(value, "debug")) {
        args->log_level = LOG_DEBUG;
    } else if (!strcasecmp(value, "info")) {
        args->log_level = LOG_INFO;
    } else if (!strcasecmp(value, "warn")) {
        args->log_level = LOG_WARN;
    } else if (!strcasecmp(value, "error")) {
        args->log_level = LOG_ERROR;
    } else if (!strcasecmp(value, "fatal")) {
        args->log_level = LOG_FATAL;
    } else if (!strcasecmp(value, "none")) {
        args->log_level = LOG_NONE;
    }
}

static void handle_stage_rootfs(char *value, void *data) {
    kernel_args_t *args = data;
    bool enabled = args->stage_rootfs != 0;

    if (parse_bool(value, &enabled)) {
        args->stage_rootfs = enabled;
    }
}

static void handle_log_color(char *value, void *data) {
    kernel_args_t *args = data;
    bool enabled = args->log_color != 0;

    if (parse_bool(value, &enabled)) {
        args->log_color = enabled;
    }
}

static void handle_log_format(char *value, void *data) {
    kernel_args_t *args = data;
    if (strlen(value) < sizeof(args->log_format)) {
        copy_value(args->log_format, sizeof(args->log_format), value);
    }
}

static void handle_video(char *value, void *data) {
    kernel_args_t *args = data;

    if (!strcasecmp(value, "graphics")) {
        args->video = VIDEO_GRAPHICS;
    } else if (!strcasecmp(value, "text")) {
        args->video = VIDEO_TEXT;
    } else if (!strcasecmp(value, "none")) {
        args->video = VIDEO_NONE;
    }
}

static void handle_vesa_width(char *value, void *data) {
    kernel_args_t *args = data;
    u16 width = 0;

    if (parse_u16(value, &width)) {
        args->vesa_width = width;
    }
}

static void handle_vesa_height(char *value, void *data) {
    kernel_args_t *args = data;
    u16 height = 0;

    if (parse_u16(value, &height)) {
        args->vesa_height = height;
    }
}

static void handle_vesa_bpp(char *value, void *data) {
    kernel_args_t *args = data;
    u16 bpp = 0;

    if (parse_u16(value, &bpp)) {
        args->vesa_bpp = bpp;
    }
}

static void handle_console(char *value, void *data) {
    kernel_args_t *args = data;
    copy_value(args->console, sizeof(args->console), value);
}

static void handle_font(char *value, void *data) {
    kernel_args_t *args = data;
    copy_value(args->font, sizeof(args->font), value);
}

static const cfg_entry_t config[] = {
    { "log.level", handle_log_level },
    { "stage_rootfs", handle_stage_rootfs },
    { "log.color", handle_log_color },
    { "log.format", handle_log_format },
    { "video", handle_video },
    { "graphics.width", handle_vesa_width },
    { "graphics.height", handle_vesa_height },
    { "graphics.bpp", handle_vesa_bpp },
    { "console", handle_console },
    { "font", handle_font },
    { "console.font", handle_font },
    { "text.font", handle_font },
    { NULL, NULL },
};

void boot_config_init(kernel_args_t *args, bool log_color) {
    if (!args) {
        return;
    }

    memset(args, 0, sizeof(*args));

    args->log_level = BOOT_DEFAULT_LOG_LEVEL;
    args->stage_rootfs = BOOT_DEFAULT_STAGE_ROOTFS;
    args->log_color = log_color;
    args->video = BOOT_DEFAULT_VIDEO;
    args->vesa_width = (u16)BOOT_DEFAULT_VESA_WIDTH;
    args->vesa_height = (u16)BOOT_DEFAULT_VESA_HEIGHT;
    args->vesa_bpp = BOOT_DEFAULT_VESA_BPP;

    copy_value(args->font, sizeof(args->font), BOOT_DEFAULT_FONT);
    copy_value(args->log_format, sizeof(args->log_format), BOOT_DEFAULT_LOG_FORMAT);
}

void boot_config_parse(char *text, kernel_args_t *args) {
    parse_cfg(text, config, args);
}
