#include <ctype.h>
#include <draw.h>
#include <errno.h>
#include <fcntl.h>
#include <gui/fb.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <ui.h>
#include <unistd.h>
#include <user/io.h>
#include <user/kv.h>

#include "background.h"
#include "color.h"
#include "cursor.h"
#include "loop.h"
#include "wm.h"

static volatile sig_atomic_t exit_requested = 0;

typedef struct {
    char background[PATH_MAX];
    char cursors[PATH_MAX];
    char font[PATH_MAX];
    wm_palette_t palette;
    u32 palette_mask;
} wm_config_t;

enum wm_cfg_palette_mask {
    WM_CFG_BACKGROUND = 1u << 0,
    WM_CFG_BORDER = 1u << 1,
    WM_CFG_TITLE = 1u << 2,
    WM_CFG_TITLE_FOCUS = 1u << 3,
    WM_CFG_CLIENT_BG = 1u << 4,
    WM_CFG_TITLE_TEXT = 1u << 5,
    WM_CFG_CLOSE_BG = 1u << 6,
    WM_CFG_CLOSE_FG = 1u << 7,
};

typedef struct {
    const char *bg_override;
    const char *cursor_override;
    int fb_fd;
} wm_startup_t;

static void _on_signal(int signum) {
    (void)signum;
    exit_requested = 1;
}

static void _on_child(int signum) {
    (void)signum;

    int saved_errno = errno;
    while (waitpid(-1, NULL, WNOHANG) > 0) {}
    errno = saved_errno;
}

static bool set_palette_color(wm_config_t *cfg, const char *key, const char *value) {
    if (!cfg || !key || !value) {
        return false;
    }

    u32 color = 0;
    if (!wm_parse_hex_color(value, &color)) {
        return false;
    }

    if (!strcmp(key, "color.background")) {
        cfg->palette.background = color;
        cfg->palette_mask |= WM_CFG_BACKGROUND;
        return true;
    }

    if (!strcmp(key, "color.border")) {
        cfg->palette.border = color;
        cfg->palette_mask |= WM_CFG_BORDER;
        return true;
    }

    if (!strcmp(key, "color.title")) {
        cfg->palette.title = color;
        cfg->palette_mask |= WM_CFG_TITLE;
        return true;
    }

    if (!strcmp(key, "color.title_focus")) {
        cfg->palette.title_focus = color;
        cfg->palette_mask |= WM_CFG_TITLE_FOCUS;
        return true;
    }

    if (!strcmp(key, "color.client_bg")) {
        cfg->palette.client_bg = color;
        cfg->palette_mask |= WM_CFG_CLIENT_BG;
        return true;
    }

    if (!strcmp(key, "color.title_text")) {
        cfg->palette.title_text = color;
        cfg->palette_mask |= WM_CFG_TITLE_TEXT;
        return true;
    }

    if (!strcmp(key, "color.close_bg")) {
        cfg->palette.close_bg = color;
        cfg->palette_mask |= WM_CFG_CLOSE_BG;
        return true;
    }

    if (!strcmp(key, "color.close_fg")) {
        cfg->palette.close_fg = color;
        cfg->palette_mask |= WM_CFG_CLOSE_FG;
        return true;
    }

    return false;
}

typedef struct {
    char key[64];
    char *value;
    size_t value_len;
} wm_cfg_pair_t;

static bool parse_config_line(char *line, wm_cfg_pair_t *pair) {
    if (!line || !pair) {
        return false;
    }

    while (*line && isspace((unsigned char)*line)) {
        line++;
    }

    if (!line[0] || line[0] == '#') {
        return false;
    }

    char *eq = strchr(line, '=');
    if (!eq) {
        return false;
    }

    char *key_start = line;
    char *key_end = eq;

    while (key_end > key_start && isspace((unsigned char)key_end[-1])) {
        key_end--;
    }

    size_t key_len = (size_t)(key_end - key_start);
    if (!key_len || key_len >= sizeof(pair->key)) {
        return false;
    }

    char *value = eq + 1;
    while (*value && isspace((unsigned char)*value)) {
        value++;
    }

    char *value_end = value + strlen(value);
    while (value_end > value && isspace((unsigned char)value_end[-1])) {
        value_end--;
    }

    size_t value_len = (size_t)(value_end - value);
    if (!value_len) {
        return false;
    }

    memcpy(pair->key, key_start, key_len);
    pair->key[key_len] = '\0';
    pair->value = value;
    pair->value_len = value_len;

    return true;
}

static void copy_config_path(char *dst, size_t dst_len, const wm_cfg_pair_t *pair) {
    size_t len = pair->value_len;
    if (len >= dst_len) {
        len = dst_len - 1;
    }

    memcpy(dst, pair->value, len);
    dst[len] = '\0';
}

static void apply_config_pair(wm_config_t *cfg, const wm_cfg_pair_t *pair) {
    if (!strcmp(pair->key, "background")) {
        copy_config_path(cfg->background, sizeof(cfg->background), pair);
        return;
    }

    if (!strcmp(pair->key, "cursors")) {
        copy_config_path(cfg->cursors, sizeof(cfg->cursors), pair);
        return;
    }

    if (!strcmp(pair->key, "font")) {
        copy_config_path(cfg->font, sizeof(cfg->font), pair);
        return;
    }

    char value[64];
    if (pair->value_len >= sizeof(value)) {
        return;
    }

    memcpy(value, pair->value, pair->value_len);
    value[pair->value_len] = '\0';
    set_palette_color(cfg, pair->key, value);
}

static void _load_wm_config(wm_config_t *cfg) {
    if (!cfg) {
        return;
    }

    cfg->background[0] = '\0';
    cfg->cursors[0] = '\0';
    cfg->font[0] = '\0';
    cfg->palette_mask = 0;

    char cfg_text[2048];
    int fd = open("/etc/wm.conf", O_RDONLY, 0);
    if (fd < 0) {
        return;
    }

    ssize_t len = kv_read_fd(fd, cfg_text, sizeof(cfg_text));
    close(fd);
    if (len <= 0) {
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

        wm_cfg_pair_t pair = { 0 };
        if (!parse_config_line(line, &pair)) {
            continue;
        }

        apply_config_pair(cfg, &pair);
    }
}

static bool _parse_fd_arg(const char *text, int *fd_out) {
    if (!text || !text[0] || !fd_out) {
        return false;
    }

    char *end = NULL;
    long value = strtol(text, &end, 10);
    if (!end || *end || value < 0 || value > INT_MAX) {
        return false;
    }

    *fd_out = (int)value;
    return true;
}

static bool _parse_args(int argc, char **argv, wm_startup_t *startup) {
    if (!startup) {
        return false;
    }

    memset(startup, 0, sizeof(*startup));
    startup->fb_fd = -1;

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

            startup->bg_override = argv[++i];
            continue;
        }

        if (!strcmp(arg, "--cursor")) {
            if (i + 1 >= argc || !argv[i + 1] || !argv[i + 1][0]) {
                io_write_str("wm: --cursor requires a path\n");
                return false;
            }

            startup->cursor_override = argv[++i];
            continue;
        }

        if (!strcmp(arg, "--fd-fb")) {
            if (i + 1 >= argc || !_parse_fd_arg(argv[i + 1], &startup->fb_fd)) {
                io_write_str("wm: --fd-fb requires a valid fd\n");
                return false;
            }
            i++;
            continue;
        }

        io_write_str("wm: unknown option\n");
        return false;
    }

    return true;
}

static void warn_bg_failed(const char *path) {
    if (!path || !path[0]) {
        return;
    }

    char line[PATH_MAX + 96];
    snprintf(line, sizeof(line), "wm: failed to load background '%s', using solid fallback\n", path);
    io_write_str(line);
}

static void warn_cursor_failed(const char *path) {
    if (!path || !path[0]) {
        return;
    }

    char line[PATH_MAX + 80];
    snprintf(line, sizeof(line), "wm: failed to load cursor '%s'\n", path);
    io_write_str(line);
}

static bool join_cursor_path(char *out, size_t out_len, const char *dir, const char *name) {
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

static const char *find_cursor(char *path, size_t path_size, const char *dir, const char *name) {
    if (!path || !path_size || !dir || !dir[0] || !name || !name[0]) {
        return NULL;
    }

    return join_cursor_path(path, path_size, dir, name) ? path : NULL;
}

static void load_cursor(wm_cursor_kind_t kind, const char *cursor_dir, const char *name, const char *fallback) {
    char path_buf[PATH_MAX];
    const char *path = find_cursor(path_buf, sizeof(path_buf), cursor_dir, name);
    if (!path) {
        path = fallback;
    }

    if (path && !wm_cursor_load_kind(kind, path)) {
        warn_cursor_failed(path);
    }
}

static void apply_palette(const wm_config_t *cfg) {
    wm_palette_t palette = *wm_palette_get();

    if (cfg->palette_mask & WM_CFG_BACKGROUND) {
        palette.background = cfg->palette.background;
    }
    if (cfg->palette_mask & WM_CFG_BORDER) {
        palette.border = cfg->palette.border;
    }
    if (cfg->palette_mask & WM_CFG_TITLE) {
        palette.title = cfg->palette.title;
    }
    if (cfg->palette_mask & WM_CFG_TITLE_FOCUS) {
        palette.title_focus = cfg->palette.title_focus;
    }
    if (cfg->palette_mask & WM_CFG_CLIENT_BG) {
        palette.client_bg = cfg->palette.client_bg;
    }
    if (cfg->palette_mask & WM_CFG_TITLE_TEXT) {
        palette.title_text = cfg->palette.title_text;
    }
    if (cfg->palette_mask & WM_CFG_CLOSE_BG) {
        palette.close_bg = cfg->palette.close_bg;
    }
    if (cfg->palette_mask & WM_CFG_CLOSE_FG) {
        palette.close_fg = cfg->palette.close_fg;
    }

    wm_palette_set(&palette);
}

static const char *pick_background(const wm_startup_t *startup, const wm_config_t *cfg) {
    if (startup->bg_override && startup->bg_override[0]) {
        return startup->bg_override;
    }

    return cfg->background[0] ? cfg->background : NULL;
}

static const char *pick_cursor(const wm_startup_t *startup, const char *cursor_dir, char *path, size_t path_size) {
    if (startup->cursor_override && startup->cursor_override[0]) {
        return startup->cursor_override;
    }

    if (join_cursor_path(path, path_size, cursor_dir, "pointer.qoi")) {
        return path;
    }

    return NULL;
}

static void load_resize_cursors(const char *cursor_dir, const char *fallback) {
    static const struct {
        wm_cursor_kind_t kind;
        const char *name;
    } specs[] = {
        {
            .kind = WM_CURSOR_RESIZE_EW,
            .name = "resize_ew.qoi",
        },
        {
            .kind = WM_CURSOR_RESIZE_NS,
            .name = "resize_ns.qoi",
        },
        {
            .kind = WM_CURSOR_RESIZE_NW,
            .name = "resize_nw.qoi",
        },
        {
            .kind = WM_CURSOR_RESIZE_SE,
            .name = "resize_se.qoi",
        },
        {
            .kind = WM_CURSOR_RESIZE_SW,
            .name = "resize_sw.qoi",
        },
    };

    for (size_t i = 0; i < sizeof(specs) / sizeof(specs[0]); i++) {
        load_cursor(specs[i].kind, cursor_dir, specs[i].name, fallback);
    }
}

static void load_assets(const wm_startup_t *startup, const wm_config_t *cfg, u32 width, u32 height) {
    const char *background = pick_background(startup, cfg);
    if (background && !wm_background_load(width, height, background)) {
        warn_bg_failed(background);
    }

    const char *cursor_dir = cfg->cursors[0] ? cfg->cursors : "/etc/cursors";

    char cursor_path[PATH_MAX];
    const char *normal_cursor = pick_cursor(startup, cursor_dir, cursor_path, sizeof(cursor_path));

    if (normal_cursor && !wm_cursor_load_kind(WM_CURSOR_NORMAL, normal_cursor)) {
        warn_cursor_failed(normal_cursor);
    }

    load_cursor(WM_CURSOR_POINTER, cursor_dir, "pointer_interact.qoi", normal_cursor);
    load_cursor(WM_CURSOR_MOVE, cursor_dir, "move.qoi", normal_cursor);

    char resize_path[PATH_MAX];
    const char *resize_cursor = find_cursor(resize_path, sizeof(resize_path), cursor_dir, "resize.qoi");

    load_resize_cursors(cursor_dir, resize_cursor);
}

typedef struct {
    int fb_fd;
    pixel_t *frame_store;
    ui_t ui;
    bool wm_inited;
    bool fb_acquired;
    bool mgr_claimed;
    fb_info_t fb_info;
    size_t frame_bytes;
} wm_app_t;

static bool open_framebuffer(wm_app_t *app, const wm_startup_t *startup) {
    if (startup->fb_fd >= 0) {
        app->fb_fd = startup->fb_fd;
    } else {
        app->fb_fd = open("/dev/fb", O_RDWR, 0);
        if (app->fb_fd < 0) {
            io_write_str("wm: open /dev/fb failed\n");
            return false;
        }
    }

    return true;
}

static bool setup_ui(wm_app_t *app) {
    if (ui_open(&app->ui, UI_OPEN_INPUT)) {
        io_write_str("wm: failed to open wsctl/keyboard/mouse\n");
        return false;
    }

    if (ui_mgr_claim(&app->ui)) {
        io_write_str("wm: failed to claim ws manager\n");
        return false;
    }

    app->mgr_claimed = true;
    return true;
}

static bool setup_framebuffer(wm_app_t *app) {
    memset(&app->fb_info, 0, sizeof(app->fb_info));

    if (ioctl(app->fb_fd, FBIOGETINFO, &app->fb_info) || !app->fb_info.available || app->fb_info.bpp != 32) {
        io_write_str("wm: unsupported framebuffer\n");
        return false;
    }

    size_t width = app->fb_info.width;
    size_t height = app->fb_info.height;
    if (!width || !height || width > WM_MAX_FB_W || height > WM_MAX_FB_H || width > SIZE_MAX / sizeof(pixel_t) ||
        height > SIZE_MAX / width) {
        io_write_str("wm: invalid framebuffer dimensions\n");
        return false;
    }

    size_t packed_row_bytes = width * sizeof(pixel_t);
    if (!app->fb_info.pitch || (size_t)app->fb_info.pitch < packed_row_bytes) {
        io_write_str("wm: invalid framebuffer pitch\n");
        return false;
    }

    size_t frame_pixels = width * height;
    if (frame_pixels > WM_MAX_FB_PIX || frame_pixels > SIZE_MAX / sizeof(pixel_t)) {
        io_write_str("wm: framebuffer too large\n");
        return false;
    }

    app->frame_bytes = frame_pixels * sizeof(pixel_t);

    app->frame_store = calloc(frame_pixels, sizeof(pixel_t));
    if (!app->frame_store) {
        io_write_str("wm: failed to allocate frame buffer\n");
        return false;
    }

    return true;
}

static bool acquire_framebuffer(wm_app_t *app) {
    if (ioctl(app->fb_fd, FBIOACQUIRE, NULL)) {
        io_write_str("wm: failed to acquire framebuffer\n");
        return false;
    }

    app->fb_acquired = true;
    return true;
}

static void setup_wm(wm_app_t *app, const wm_startup_t *startup, wm_config_t *cfg) {
    _load_wm_config(cfg);

    if (cfg->font[0] && !draw_set_font_path(cfg->font)) {
        io_write_str("wm: invalid font path in config\n");
    }

    apply_palette(cfg);

    wm_init();
    app->wm_inited = true;

    load_assets(startup, cfg, app->fb_info.width, app->fb_info.height);
}

static void wm_app_cleanup(wm_app_t *app) {
    wm_background_unload();
    wm_cursor_unload();

    if (app->mgr_claimed) {
        ui_mgr_release(&app->ui);
    }

    if (app->wm_inited) {
        wm_cleanup_all_windows();
        wm_destroy();
    }

    ui_close(&app->ui);

    if (app->fb_acquired) {
        ioctl(app->fb_fd, FBIORELEASE, NULL);
    }

    if (app->fb_fd >= 0) {
        close(app->fb_fd);
    }

    if (app->frame_store) {
        free(app->frame_store);
    }
}

int main(int argc, char **argv) {
    wm_startup_t startup = { 0 };
    if (!_parse_args(argc, argv, &startup)) {
        return 1;
    }

    signal(SIGINT, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    signal(SIGCHLD, _on_child);
    signal(SIGTERM, _on_signal);
    signal(SIGHUP, _on_signal);

    wm_app_t app = {
        .fb_fd = -1,
    };
    int exit_status = 1;

    if (!open_framebuffer(&app, &startup)) {
        goto out;
    }

    if (!setup_ui(&app)) {
        goto out;
    }

    if (!setup_framebuffer(&app)) {
        goto out;
    }

    wm_config_t cfg = { 0 };
    setup_wm(&app, &startup, &cfg);

    if (!acquire_framebuffer(&app)) {
        goto out;
    }

    wm_loop(&app.ui, app.fb_fd, &app.fb_info, app.frame_store, app.frame_bytes, &exit_requested);
    exit_status = 0;

out:
    wm_app_cleanup(&app);

    return exit_status;
}
