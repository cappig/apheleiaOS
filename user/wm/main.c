#include <ctype.h>
#include <fcntl.h>
#include <gui/fb.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <ui.h>
#include <unistd.h>
#include <user/io.h>
#include <user/kv.h>

#include "wm.h"
#include "wm_background.h"
#include "wm_cursor.h"
#include "wm_loop.h"

static volatile sig_atomic_t exit_requested = 0;

typedef struct {
    char background[PATH_MAX];
    char cursors[PATH_MAX];
    wm_palette_t palette;
    u32 palette_mask;
} wm_config_t;

enum wm_cfg_palette_mask {
    WM_CFG_PAL_BACKGROUND = 1u << 0,
    WM_CFG_PAL_BORDER = 1u << 1,
    WM_CFG_PAL_TITLE = 1u << 2,
    WM_CFG_PAL_TITLE_FOCUS = 1u << 3,
    WM_CFG_PAL_CLIENT_BG = 1u << 4,
    WM_CFG_PAL_TITLE_TEXT = 1u << 5,
    WM_CFG_PAL_CLOSE_BG = 1u << 6,
    WM_CFG_PAL_CLOSE_FG = 1u << 7,
};

static void _on_signal(int signum) {
    (void)signum;
    exit_requested = 1;
}

static int _hex_nibble(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }

    if (ch >= 'a' && ch <= 'f') {
        return 10 + (ch - 'a');
    }

    if (ch >= 'A' && ch <= 'F') {
        return 10 + (ch - 'A');
    }

    return -1;
}

static bool _parse_hex_color(const char *text, u32 *color_out) {
    if (!text || !color_out) {
        return false;
    }

    const char *pos = text;
    if (pos[0] == '#') {
        pos++;
    } else if (pos[0] == '0' && (pos[1] == 'x' || pos[1] == 'X')) {
        pos += 2;
    }

    u32 value = 0;
    size_t digits = 0;
    while (digits < 8) {
        int nib = _hex_nibble(pos[digits]);
        if (nib < 0) {
            break;
        }

        value = (value << 4) | (u32)nib;
        digits++;
    }

    if (pos[digits] != '\0') {
        return false;
    }

    if (digits == 6) {
        *color_out = value;
        return true;
    }

    if (digits == 8) {
        *color_out = value & 0x00ffffffU;
        return true;
    }

    return false;
}

static bool _cfg_set_palette_color(wm_config_t *cfg, const char *key, const char *value) {
    if (!cfg || !key || !value) {
        return false;
    }

    u32 color = 0;
    if (!_parse_hex_color(value, &color)) {
        return false;
    }

    if (!strcmp(key, "color.background")) {
        cfg->palette.background = color;
        cfg->palette_mask |= WM_CFG_PAL_BACKGROUND;
        return true;
    }

    if (!strcmp(key, "color.border")) {
        cfg->palette.border = color;
        cfg->palette_mask |= WM_CFG_PAL_BORDER;
        return true;
    }

    if (!strcmp(key, "color.title")) {
        cfg->palette.title = color;
        cfg->palette_mask |= WM_CFG_PAL_TITLE;
        return true;
    }

    if (!strcmp(key, "color.title_focus")) {
        cfg->palette.title_focus = color;
        cfg->palette_mask |= WM_CFG_PAL_TITLE_FOCUS;
        return true;
    }

    if (!strcmp(key, "color.client_bg")) {
        cfg->palette.client_bg = color;
        cfg->palette_mask |= WM_CFG_PAL_CLIENT_BG;
        return true;
    }

    if (!strcmp(key, "color.title_text")) {
        cfg->palette.title_text = color;
        cfg->palette_mask |= WM_CFG_PAL_TITLE_TEXT;
        return true;
    }

    if (!strcmp(key, "color.close_bg")) {
        cfg->palette.close_bg = color;
        cfg->palette_mask |= WM_CFG_PAL_CLOSE_BG;
        return true;
    }

    if (!strcmp(key, "color.close_fg")) {
        cfg->palette.close_fg = color;
        cfg->palette_mask |= WM_CFG_PAL_CLOSE_FG;
        return true;
    }

    return false;
}

static void _load_wm_config(wm_config_t *cfg) {
    if (!cfg) {
        return;
    }

    cfg->background[0] = '\0';
    cfg->cursors[0] = '\0';
    cfg->palette_mask = 0;

    char cfg_text[2048];
    if (kv_read_file("/etc/wm.conf", cfg_text, sizeof(cfg_text)) <= 0) {
        return;
    }

    char *pos = cfg_text;
    while (*pos) {
        char *line = pos;
        char *nl = strchr(pos, '\n');
        if (nl) {
            *nl = '\0';
            pos = nl + 1;
        } else {
            pos += strlen(pos);
        }

        while (*line && isspace((unsigned char)*line)) {
            line++;
        }

        if (!line[0] || line[0] == '#') {
            continue;
        }

        char *eq = strchr(line, '=');
        if (!eq) {
            continue;
        }

        char *key_start = line;
        char *key_end = eq;
        while (key_end > key_start && isspace((unsigned char)key_end[-1])) {
            key_end--;
        }

        char *value_start = eq + 1;
        while (*value_start && isspace((unsigned char)*value_start)) {
            value_start++;
        }

        char *value_end = value_start + strlen(value_start);
        while (value_end > value_start && isspace((unsigned char)value_end[-1])) {
            value_end--;
        }
        size_t value_len = (size_t)(value_end - value_start);
        if (!value_len) {
            continue;
        }

        size_t key_len = (size_t)(key_end - key_start);
        char key[64];
        if (!key_len || key_len >= sizeof(key)) {
            continue;
        }
        memcpy(key, key_start, key_len);
        key[key_len] = '\0';

        char *out = NULL;
        size_t out_len = 0;
        char value_buf[64];

        if (!strcmp(key, "background")) {
            out = cfg->background;
            out_len = sizeof(cfg->background);
        } else if (!strcmp(key, "cursors")) {
            out = cfg->cursors;
            out_len = sizeof(cfg->cursors);
        } else if (value_len < sizeof(value_buf)) {
            memcpy(value_buf, value_start, value_len);
            value_buf[value_len] = '\0';
            if (_cfg_set_palette_color(cfg, key, value_buf)) {
                continue;
            }
        } else {
            continue;
        }

        if (value_len >= out_len) {
            value_len = out_len - 1;
        }

        memcpy(out, value_start, value_len);
        out[value_len] = '\0';
    }
}

static bool
_parse_args(int argc, char **argv, const char **bg_override_out, const char **cursor_override_out) {
    if (!bg_override_out || !cursor_override_out) {
        return false;
    }

    *bg_override_out = NULL;
    *cursor_override_out = NULL;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (!arg || !arg[0]) {
            continue;
        }

        if (!strcmp(arg, "--bg")) {
            if (i + 1 >= argc || !argv[i + 1] || !argv[i + 1][0]) {
                io_write_str("wm: --bg requires a path\n");
                return false;
            }

            *bg_override_out = argv[++i];
            continue;
        }

        if (!strcmp(arg, "--cursor")) {
            if (i + 1 >= argc || !argv[i + 1] || !argv[i + 1][0]) {
                io_write_str("wm: --cursor requires a path\n");
                return false;
            }

            *cursor_override_out = argv[++i];
            continue;
        }

        io_write_str("wm: unknown option\n");
        return false;
    }

    return true;
}

static void _warn_background_failed(const char *path) {
    if (!path || !path[0]) {
        return;
    }

    char line[PATH_MAX + 96];
    snprintf(
        line, sizeof(line), "wm: failed to load background '%s', using solid fallback\\n", path
    );
    io_write_str(line);
}

static void _warn_cursor_failed(const char *path) {
    if (!path || !path[0]) {
        return;
    }

    char line[PATH_MAX + 80];
    snprintf(line, sizeof(line), "wm: failed to load cursor '%s'\\n", path);
    io_write_str(line);
}

static bool _cursor_path_join(char *out, size_t out_len, const char *dir, const char *name) {
    if (!out || !out_len || !dir || !dir[0] || !name || !name[0]) {
        return false;
    }

    size_t dir_len = strlen(dir);
    int n = 0;
    if (dir_len && dir[dir_len - 1] == '/') {
        n = snprintf(out, out_len, "%s%s", dir, name);
    } else {
        n = snprintf(out, out_len, "%s/%s", dir, name);
    }

    return n > 0 && (size_t)n < out_len;
}

int main(int argc, char **argv) {
    int ret = 1;
    int fb_fd = -1;
    pixel_t *frame_store = NULL;

    ui_t ui = {0};

    bool wm_inited = false;
    bool fb_acquired = false;
    bool mgr_claimed = false;

    const char *bg_override = NULL;
    const char *cursor_override = NULL;
    if (!_parse_args(argc, argv, &bg_override, &cursor_override)) {
        return 1;
    }

    signal(SIGINT, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    signal(SIGTERM, _on_signal);
    signal(SIGHUP, _on_signal);

    fb_fd = open("/dev/fb", O_RDWR, 0);

    if (fb_fd < 0) {
        io_write_str("wm: open /dev/fb failed\n");
        goto out;
    }

    fb_info_t fb_info = {0};
    if (ioctl(fb_fd, FBIOGETINFO, &fb_info) || !fb_info.available || fb_info.bpp != 32) {
        io_write_str("wm: unsupported framebuffer\n");
        goto out;
    }

    size_t packed_row_bytes = (size_t)fb_info.width * 4;
    if (!fb_info.pitch || (size_t)fb_info.pitch < packed_row_bytes) {
        io_write_str("wm: invalid framebuffer pitch\n");
        goto out;
    }

    if (ioctl(fb_fd, FBIOACQUIRE, NULL)) {
        io_write_str("wm: failed to acquire framebuffer\n");
        goto out;
    }

    fb_acquired = true;

    if (ui_open(&ui, UI_OPEN_INPUT)) {
        io_write_str("wm: failed to open wsctl/keyboard/mouse\n");
        goto out;
    }

    if (ui_mgr_claim(&ui)) {
        io_write_str("wm: failed to claim ws manager\n");
        goto out;
    }

    mgr_claimed = true;

    size_t frame_pixels = (size_t)fb_info.width * (size_t)fb_info.height;
    size_t frame_bytes = frame_pixels * sizeof(pixel_t);

    if (frame_pixels > WM_MAX_FB_PIX) {
        io_write_str("wm: framebuffer too large\n");
        goto out;
    }

    frame_store = malloc(frame_bytes);
    if (!frame_store) {
        io_write_str("wm: failed to allocate frame buffer\n");
        goto out;
    }

    wm_config_t cfg = {0};
    _load_wm_config(&cfg);

    wm_palette_t palette = *wm_palette_get();

    if (cfg.palette_mask & WM_CFG_PAL_BACKGROUND) {
        palette.background = cfg.palette.background;
    }
    if (cfg.palette_mask & WM_CFG_PAL_BORDER) {
        palette.border = cfg.palette.border;
    }
    if (cfg.palette_mask & WM_CFG_PAL_TITLE) {
        palette.title = cfg.palette.title;
    }
    if (cfg.palette_mask & WM_CFG_PAL_TITLE_FOCUS) {
        palette.title_focus = cfg.palette.title_focus;
    }
    if (cfg.palette_mask & WM_CFG_PAL_CLIENT_BG) {
        palette.client_bg = cfg.palette.client_bg;
    }
    if (cfg.palette_mask & WM_CFG_PAL_TITLE_TEXT) {
        palette.title_text = cfg.palette.title_text;
    }
    if (cfg.palette_mask & WM_CFG_PAL_CLOSE_BG) {
        palette.close_bg = cfg.palette.close_bg;
    }
    if (cfg.palette_mask & WM_CFG_PAL_CLOSE_FG) {
        palette.close_fg = cfg.palette.close_fg;
    }

    wm_palette_set(&palette);

    const char *bg_path = NULL;
    if (bg_override && bg_override[0]) {
        bg_path = bg_override;
    } else if (cfg.background[0]) {
        bg_path = cfg.background;
    }

    const char *cursors_dir = cfg.cursors[0] ? cfg.cursors : "/etc/cursors";

    char cursor_default_path[PATH_MAX];
    const char *cursor_path = NULL;

    if (cursor_override && cursor_override[0]) {
        cursor_path = cursor_override;
    } else if (_cursor_path_join(
                   cursor_default_path, sizeof(cursor_default_path), cursors_dir, "pointer.ppm"
               )) {
        cursor_path = cursor_default_path;
    }

    wm_init();
    wm_inited = true;

    if (bg_path && !wm_background_load(fb_info.width, fb_info.height, bg_path)) {
        _warn_background_failed(bg_path);
    }

    if (cursor_path && !wm_cursor_load(cursor_path)) {
        _warn_cursor_failed(cursor_path);
    }

    char resize_fallback_path[PATH_MAX];
    const char *resize_fallback = NULL;
    if (_cursor_path_join(resize_fallback_path, sizeof(resize_fallback_path), cursors_dir, "resize.ppm")) {
        resize_fallback = resize_fallback_path;
    }

    typedef struct {
        wm_cursor_kind_t kind;
        const char *name;
    } resize_cursor_spec_t;

    const resize_cursor_spec_t resize_specs[] = {
        {WM_CURSOR_RESIZE_EW, "resize_ew.ppm"},
        {WM_CURSOR_RESIZE_NS, "resize_ns.ppm"},
        {WM_CURSOR_RESIZE_NW, "resize_nw.ppm"},
        {WM_CURSOR_RESIZE_SE, "resize_se.ppm"},
        {WM_CURSOR_RESIZE_SW, "resize_sw.ppm"},
    };

    for (size_t i = 0; i < (sizeof(resize_specs) / sizeof(resize_specs[0])); i++) {
        char cursor_kind_path[PATH_MAX];
        const char *load_path = NULL;

        if (_cursor_path_join(
                cursor_kind_path, sizeof(cursor_kind_path), cursors_dir, resize_specs[i].name
            )) {
            load_path = cursor_kind_path;
        } else {
            load_path = resize_fallback;
        }

        if (load_path && !wm_cursor_load_kind(resize_specs[i].kind, load_path)) {
            _warn_cursor_failed(load_path);
        }
    }

    wm_loop(&ui, fb_fd, &fb_info, frame_store, frame_bytes, &exit_requested);
    ret = 0;

out:
    wm_background_unload();
    wm_cursor_unload();

    if (mgr_claimed) {
        ui_mgr_release(&ui);
    }

    if (wm_inited) {
        wm_cleanup_all_windows();
        wm_destroy();
    }

    ui_close(&ui);

    if (fb_acquired) {
        ioctl(fb_fd, FBIORELEASE, NULL);
    }

    if (fb_fd >= 0) {
        close(fb_fd);
    }

    if (frame_store) {
        free(frame_store);
    }

    return ret;
}
