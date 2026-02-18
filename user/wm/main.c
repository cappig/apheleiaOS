#include <ctype.h>
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
#include <ui.h>
#include <unistd.h>
#include <user/io.h>

#include "wm_background.h"
#include "wm.h"
#include "wm_loop.h"

static volatile sig_atomic_t exit_requested = 0;

static void _on_signal(int signum) {
    (void)signum;
    exit_requested = 1;
}

static ssize_t _read_text_file(const char* path, char* out, size_t out_len) {
    if (!path || !out || out_len < 2)
        return -1;

    int fd = open(path, O_RDONLY, 0);
    if (fd < 0)
        return -1;

    size_t used = 0;
    while (used + 1 < out_len) {
        ssize_t n = read(fd, out + used, out_len - 1 - used);
        if (n < 0) {
            if (errno == EINTR)
                continue;

            close(fd);
            return -1;
        }

        if (!n)
            break;

        used += (size_t)n;
    }

    close(fd);
    out[used] = '\0';
    return (ssize_t)used;
}

static bool _load_wm_config_background(char* out, size_t out_len) {
    if (!out || !out_len)
        return false;

    out[0] = '\0';

    char cfg_text[2048];
    if (_read_text_file("/etc/wm.conf", cfg_text, sizeof(cfg_text)) <= 0)
        return false;

    char* pos = cfg_text;
    while (*pos) {
        char* line = pos;
        char* nl = strchr(pos, '\n');
        if (nl) {
            *nl = '\0';
            pos = nl + 1;
        } else {
            pos += strlen(pos);
        }

        while (*line && isspace((unsigned char)*line))
            line++;

        if (!line[0] || line[0] == '#')
            continue;

        char* eq = strchr(line, '=');
        if (!eq)
            continue;

        char* key_start = line;
        char* key_end = eq;
        while (key_end > key_start && isspace((unsigned char)key_end[-1]))
            key_end--;

        char* value_start = eq + 1;
        while (*value_start && isspace((unsigned char)*value_start))
            value_start++;

        char* value_end = value_start + strlen(value_start);
        while (value_end > value_start && isspace((unsigned char)value_end[-1]))
            value_end--;

        size_t key_len = (size_t)(key_end - key_start);
        if (key_len != strlen("background") || strncmp(key_start, "background", key_len))
            continue;

        size_t value_len = (size_t)(value_end - value_start);
        if (!value_len)
            continue;

        if (value_len >= out_len)
            value_len = out_len - 1;

        memcpy(out, value_start, value_len);
        out[value_len] = '\0';
    }

    return out[0] != '\0';
}

static bool _parse_args(int argc, char** argv, const char** bg_override_out) {
    if (!bg_override_out)
        return false;

    *bg_override_out = NULL;

    for (int i = 1; i < argc; i++) {
        const char* arg = argv[i];
        if (!arg || !arg[0])
            continue;

        if (!strcmp(arg, "--bg")) {
            if (i + 1 >= argc || !argv[i + 1] || !argv[i + 1][0]) {
                io_write_str("wm: --bg requires a path\n");
                return false;
            }

            *bg_override_out = argv[++i];
            continue;
        }

        io_write_str("wm: unknown option\n");
        return false;
    }

    return true;
}

static void _warn_background_failed(const char* path) {
    if (!path || !path[0])
        return;

    char line[PATH_MAX + 96];
    snprintf(
        line,
        sizeof(line),
        "wm: failed to load background '%s', using solid fallback\\n",
        path
    );
    io_write_str(line);
}

int main(int argc, char** argv) {
    int ret = 1;
    int fb_fd = -1;
    u32* frame_store = NULL;

    ui_t ui = {0};

    bool wm_inited = false;
    bool fb_acquired = false;
    bool mgr_claimed = false;

    const char* bg_override = NULL;
    if (!_parse_args(argc, argv, &bg_override))
        return 1;

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
        io_write_str("wm: failed to open input/wsctl\n");
        goto out;
    }

    if (ui_mgr_claim(&ui)) {
        io_write_str("wm: failed to claim ws manager\n");
        goto out;
    }

    mgr_claimed = true;

    size_t frame_pixels = (size_t)fb_info.width * (size_t)fb_info.height;
    size_t frame_bytes = frame_pixels * 4;

    if (frame_pixels > WM_MAX_FB_PIX) {
        io_write_str("wm: framebuffer too large\n");
        goto out;
    }

    frame_store = malloc(frame_bytes);
    if (!frame_store) {
        io_write_str("wm: failed to allocate frame buffer\n");
        goto out;
    }

    char bg_config[PATH_MAX] = {0};
    (void)_load_wm_config_background(bg_config, sizeof(bg_config));

    const char* bg_path = NULL;
    if (bg_override && bg_override[0])
        bg_path = bg_override;
    else if (bg_config[0])
        bg_path = bg_config;

    wm_init();
    wm_inited = true;

    if (bg_path && !wm_background_load(fb_info.width, fb_info.height, bg_path))
        _warn_background_failed(bg_path);

    wm_loop(&ui, fb_fd, &fb_info, frame_store, frame_bytes, &exit_requested);
    ret = 0;

out:
    wm_background_unload();

    if (mgr_claimed)
        ui_mgr_release(&ui);

    if (wm_inited) {
        wm_cleanup_all_windows();
        wm_destroy();
    }

    ui_close(&ui);

    if (fb_acquired)
        ioctl(fb_fd, FBIORELEASE, NULL);

    if (fb_fd >= 0)
        close(fb_fd);

    if (frame_store)
        free(frame_store);

    return ret;
}
