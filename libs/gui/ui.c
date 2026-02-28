#include "ui.h"

#include <errno.h>
#include <fcntl.h>
#include <input/kbd.h>
#include <input/mouse.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

#define UI_PENDING_EVENTS_CAP 3

static const char *env_lookup(const char *key) {
    if (!key || !key[0] || !environ) {
        return NULL;
    }

    size_t key_len = strlen(key);
    if (!key_len) {
        return NULL;
    }

    for (char **env = environ; *env; env++) {
        if (strncmp(*env, key, key_len)) {
            continue;
        }

        if ((*env)[key_len] != '=') {
            continue;
        }

        return *env + key_len + 1;
    }

    return NULL;
}

static bool parse_env_u32(const char *key, u32 *out) {
    if (!key || !out) {
        errno = EINVAL;
        return false;
    }

    const char *value = env_lookup(key);
    if (!value || !value[0]) {
        errno = ENOENT;
        return false;
    }

    char *end = NULL;
    long parsed = strtol(value, &end, 10);
    if (!end || *end || parsed < 0) {
        errno = EINVAL;
        return false;
    }

    *out = (u32)parsed;
    return true;
}

static void close_fd(int *fd) {
    if (!fd || *fd < 0) {
        return;
    }

    close(*fd);
    *fd = -1;
}

static void window_sync_framebuffer(window_t *window) {
    if (!window) {
        return;
    }

    window->framebuffer.pixels = window->pixels;
    window->framebuffer.width = window->width;
    window->framebuffer.height = window->height;
    window->framebuffer.stride = window->stride;
    window->framebuffer.pixel_count = window->pixels_count;
}

static u64 _input_timestamp_ms(void) {
    time_t now = time(NULL);
    if (now <= 0) {
        return 0;
    }

    return (u64)now * 1000ULL;
}

static void _update_key_modifiers(ui_t *ui, const key_event *event) {
    if (!ui || !event) {
        return;
    }

    bool down = (event->type & KEY_ACTION) != 0;

    switch (event->code) {
    case KBD_LEFT_SHIFT:
    case KBD_RIGHT_SHIFT:
        if (down) {
            ui->key_modifiers |= INPUT_MOD_SHIFT;
        } else {
            ui->key_modifiers &= ~INPUT_MOD_SHIFT;
        }
        break;
    case KBD_LEFT_CTRL:
    case KBD_RIGHT_CTRL:
        if (down) {
            ui->key_modifiers |= INPUT_MOD_CTRL;
        } else {
            ui->key_modifiers &= ~INPUT_MOD_CTRL;
        }
        break;
    case KBD_LEFT_ALT:
    case KBD_RIGHT_ALT:
        if (down) {
            ui->key_modifiers |= INPUT_MOD_ALT;
        } else {
            ui->key_modifiers &= ~INPUT_MOD_ALT;
        }
        break;
    case KBD_CAPSLOCK:
        if (down) {
            ui->key_modifiers ^= INPUT_MOD_CAPS;
        }
        break;
    default:
        break;
    }
}

static void
_translate_key_event(ui_t *ui, const key_event *raw, input_event_t *out) {
    if (!ui || !raw || !out) {
        return;
    }

    _update_key_modifiers(ui, raw);

    memset(out, 0, sizeof(*out));
    out->timestamp_ms = _input_timestamp_ms();
    out->type = INPUT_EVENT_KEY;
    out->source = raw->source;
    out->keycode = raw->code;
    out->action = (raw->type & KEY_ACTION) ? 1U : 0U;
    out->modifiers = ui->key_modifiers;
}

static size_t _translate_mouse_event(
    ui_t *ui,
    const mouse_event *raw,
    input_event_t *out,
    size_t out_cap
) {
    if (!ui || !raw || !out || !out_cap) {
        return 0;
    }

    size_t produced = 0;
    u64 timestamp = _input_timestamp_ms();

    if ((raw->delta_x || raw->delta_y) && produced < out_cap) {
        input_event_t *move = &out[produced++];

        memset(move, 0, sizeof(*move));

        move->timestamp_ms = timestamp;
        move->type = INPUT_EVENT_MOUSE_MOVE;
        move->source = raw->source;
        move->dx = raw->delta_x;
        move->dy = raw->delta_y;
        move->buttons = raw->buttons;
    }

    if (raw->wheel && produced < out_cap) {
        input_event_t *wheel = &out[produced++];

        memset(wheel, 0, sizeof(*wheel));

        wheel->timestamp_ms = timestamp;
        wheel->type = INPUT_EVENT_MOUSE_WHEEL;
        wheel->source = raw->source;
        wheel->buttons = raw->buttons;
        wheel->wheel = raw->wheel;
    }

    if (ui->mouse_buttons == raw->buttons) {
        return produced;
    }

    ui->mouse_buttons = raw->buttons;

    if (produced >= out_cap) {
        return produced;
    }

    input_event_t *buttons = &out[produced++];

    memset(buttons, 0, sizeof(*buttons));

    buttons->timestamp_ms = timestamp;
    buttons->type = INPUT_EVENT_MOUSE_BUTTON;
    buttons->source = raw->source;
    buttons->buttons = raw->buttons;

    return produced;
}

static void _pending_reset(ui_t *ui) {
    if (!ui) {
        return;
    }

    ui->pending_head = 0;
    ui->pending_count = 0;
    memset(ui->pending_events, 0, sizeof(ui->pending_events));
}

static bool _pending_pop(ui_t *ui, input_event_t *out) {
    if (!ui || !out || !ui->pending_count) {
        return false;
    }

    *out = ui->pending_events[ui->pending_head];
    ui->pending_head = (u8)((ui->pending_head + 1U) % UI_PENDING_EVENTS_CAP);
    ui->pending_count--;
    return true;
}

static bool _pending_push(ui_t *ui, const input_event_t *event) {
    if (!ui || !event || ui->pending_count >= UI_PENDING_EVENTS_CAP) {
        return false;
    }

    u8 tail = (u8)((ui->pending_head + ui->pending_count) % UI_PENDING_EVENTS_CAP);
    ui->pending_events[tail] = *event;
    ui->pending_count++;
    return true;
}

static int
ui_simple(ui_t *ui, unsigned long request, u32 id, i32 x, i32 y, u32 flags) {
    if (!ui || ui->ctl_fd < 0) {
        errno = EINVAL;
        return -1;
    }

    ws_cmd_t cmd = {0};
    cmd.id = id;
    cmd.x = x;
    cmd.y = y;
    cmd.flags = flags;

    return ioctl(ui->ctl_fd, request, &cmd);
}

static void window_reset_runtime(window_t *window) {
    if (!window) {
        return;
    }

    close_fd(&window->fb_fd);
    close_fd(&window->ev_fd);
    window->pixels = NULL;
    window->pixels_count = 0;
    window->pixels_capacity = 0;
    window_sync_framebuffer(window);
}

static void window_unmap_pixels(window_t *window) {
    if (!window || !window->pixels || !window->pixels_capacity) {
        return;
    }

    free(window->pixels);

    window->pixels = NULL;
    window->pixels_count = 0;
    window->pixels_capacity = 0;
    window_sync_framebuffer(window);
}

static void window_reset(window_t *window, ui_t *ui) {
    if (!window) {
        return;
    }

    window_unmap_pixels(window);
    window_reset_runtime(window);

    window->ui = ui;
    window->id = 0;
    window->width = 0;
    window->height = 0;
    window->stride = 0;
    window_sync_framebuffer(window);
}

static int window_open_fds(window_t *window) {
    if (!window) {
        errno = EINVAL;
        return -1;
    }

    char fb_path[64];
    char ev_path[64];

    snprintf(fb_path, sizeof(fb_path), "/dev/ws/%u/fb", window->id);
    snprintf(ev_path, sizeof(ev_path), "/dev/ws/%u/ev", window->id);

    window->fb_fd = open(fb_path, O_RDWR, 0);
    if (window->fb_fd < 0) {
        return -1;
    }

    window->ev_fd = open(ev_path, O_RDONLY | O_NONBLOCK, 0);
    if (window->ev_fd >= 0) {
        return 0;
    }

    int saved = errno;
    close_fd(&window->fb_fd);
    errno = saved;
    return -1;
}

int ui_open(ui_t *ui, u32 flags) {
    if (!ui) {
        errno = EINVAL;
        return -1;
    }

    ui->ctl_fd = -1;
    ui->mgr_fd = -1;
    ui->keyboard_fd = -1;
    ui->mouse_fd = -1;
    ui->key_modifiers = 0;
    ui->mouse_buttons = 0;
    ui->input_round_robin = false;
    _pending_reset(ui);

    ui->ctl_fd = open("/dev/wsctl", O_RDWR | O_NONBLOCK, 0);
    if (ui->ctl_fd < 0) {
        return -1;
    }

    if (!(flags & UI_OPEN_INPUT)) {
        return 0;
    }

    ui->keyboard_fd = open("/dev/keyboard", O_RDONLY | O_NONBLOCK, 0);
    if (ui->keyboard_fd < 0) {
        int saved = errno;
        ui_close(ui);
        errno = saved;
        return -1;
    }

    ui->mouse_fd = open("/dev/mouse", O_RDONLY | O_NONBLOCK, 0);
    if (ui->mouse_fd >= 0) {
        return 0;
    }

    int saved = errno;
    ui_close(ui);
    errno = saved;
    return -1;
}

void ui_close(ui_t *ui) {
    if (!ui) {
        return;
    }

    close_fd(&ui->mouse_fd);
    close_fd(&ui->keyboard_fd);
    close_fd(&ui->mgr_fd);
    close_fd(&ui->ctl_fd);
    _pending_reset(ui);
}

ssize_t ui_input(ui_t *ui, input_event_t *events, size_t count) {
    if (!ui || !events || !count || ui->keyboard_fd < 0 || ui->mouse_fd < 0) {
        errno = EINVAL;
        return -1;
    }

    size_t produced = 0;

    while (produced < count && _pending_pop(ui, &events[produced])) {
        produced++;
    }

    if (produced >= count) {
        return (ssize_t)(produced * sizeof(*events));
    }

    for (;;) {
        if (produced >= count) {
            break;
        }

        bool consumed = false;
        bool keyboard_first = !ui->input_round_robin;

        for (int pass = 0; pass < 2; pass++) {
            bool read_keyboard = (pass == 0) ? keyboard_first : !keyboard_first;

            if (read_keyboard) {
                key_event raw = {0};
                ssize_t n = read(ui->keyboard_fd, &raw, sizeof(raw));

                if (n == (ssize_t)sizeof(raw)) {
                    _translate_key_event(ui, &raw, &events[produced++]);
                    ui->input_round_robin = true;
                    consumed = true;
                    break;
                }

                if (n > 0) {
                    // Drop truncated keyboard bytes and keep draining.
                    continue;
                }

                if (n < 0 && errno != EAGAIN) {
                    return produced ? (ssize_t)(produced * sizeof(*events)) : -1;
                }

                continue;
            }

            mouse_event raw = {0};
            ssize_t n = read(ui->mouse_fd, &raw, sizeof(raw));

            if (n == (ssize_t)sizeof(raw)) {
                input_event_t converted[3];
                size_t event_count =
                    _translate_mouse_event(
                        ui,
                        &raw,
                        converted,
                        sizeof(converted) / sizeof(converted[0])
                    );

                if (event_count) {
                    for (size_t i = 0; i < event_count; i++) {
                        if (produced < count) {
                            events[produced++] = converted[i];
                        } else {
                            (void)_pending_push(ui, &converted[i]);
                        }
                    }

                    ui->input_round_robin = false;
                    consumed = true;
                    break;
                }

                continue;
            }

            if (n > 0) {
                // Drop truncated mouse bytes and keep draining.
                continue;
            }

            if (n < 0 && errno != EAGAIN) {
                return produced ? (ssize_t)(produced * sizeof(*events)) : -1;
            }
        }

        if (!consumed) {
            break;
        }

        while (produced < count && _pending_pop(ui, &events[produced])) {
            produced++;
        }
    }

    if (produced) {
        return (ssize_t)(produced * sizeof(*events));
    }

    errno = EAGAIN;
    return -1;
}

int ui_mgr_claim(ui_t *ui) {
    if (!ui) {
        errno = EINVAL;
        return -1;
    }

    if (ui_simple(ui, WSIOC_CLAIM_MANAGER, 0, 0, 0, 0) < 0) {
        return -1;
    }

    if (ui->mgr_fd >= 0) {
        return 0;
    }

    ui->mgr_fd = open("/dev/wsmgr", O_RDONLY | O_NONBLOCK, 0);
    if (ui->mgr_fd >= 0) {
        return 0;
    }

    int saved = errno;
    ui_simple(ui, WSIOC_RELEASE_MANAGER, 0, 0, 0, 0);
    errno = saved;
    return -1;
}

int ui_mgr_release(ui_t *ui) {
    if (!ui) {
        errno = EINVAL;
        return -1;
    }

    close_fd(&ui->mgr_fd);
    return ui_simple(ui, WSIOC_RELEASE_MANAGER, 0, 0, 0, 0);
}

int ui_mgr_transfer(ui_t *ui, pid_t pid) {
    if (!ui || ui->ctl_fd < 0 || pid <= 0) {
        errno = EINVAL;
        return -1;
    }

    ws_cmd_t cmd = {0};
    cmd.pid = pid;
    return ioctl(ui->ctl_fd, WSIOC_TRANSFER_MANAGER, &cmd);
}

ssize_t ui_mgr_events(ui_t *ui, ws_event_t *events, size_t count) {
    if (!ui || ui->mgr_fd < 0 || !events || !count) {
        errno = EINVAL;
        return -1;
    }

    return read(ui->mgr_fd, events, count * sizeof(*events));
}

int ui_mgr_focus(ui_t *ui, u32 id) {
    return ui_simple(ui, WSIOC_SET_FOCUS, id, 0, 0, 0);
}

int ui_mgr_move(ui_t *ui, u32 id, i32 x, i32 y) {
    return ui_simple(ui, WSIOC_SET_POS, id, x, y, 0);
}

int ui_mgr_resize(ui_t *ui, u32 id, u32 width, u32 height) {
    if (!ui || ui->ctl_fd < 0 || !width || !height) {
        errno = EINVAL;
        return -1;
    }

    ws_cmd_t cmd = {0};
    cmd.id = id;
    cmd.width = width;
    cmd.height = height;

    return ioctl(ui->ctl_fd, WSIOC_SET_SIZE, &cmd);
}

int ui_mgr_raise(ui_t *ui, u32 id, u32 z) {
    return ui_simple(ui, WSIOC_SET_Z, id, 0, 0, z);
}

int ui_mgr_close(ui_t *ui, u32 id) {
    return ui_simple(ui, WSIOC_CLOSE, id, 0, 0, 0);
}

int ui_mgr_send(ui_t *ui, u32 id, const input_event_t *event) {
    if (!ui || ui->ctl_fd < 0 || !event) {
        errno = EINVAL;
        return -1;
    }

    ws_cmd_t cmd = {0};
    cmd.id = id;
    memcpy(&cmd.input, event, sizeof(cmd.input));

    return ioctl(ui->ctl_fd, WSIOC_SEND_INPUT, &cmd);
}

int window_alloc(
    ui_t *ui,
    window_t *window,
    u32 width,
    u32 height,
    const char *title
) {
    if (!ui || !window) {
        errno = EINVAL;
        return -1;
    }

    window_reset(window, ui);

    ws_cmd_t cmd = {0};
    cmd.width = width;
    cmd.height = height;

    if (title) {
        snprintf(cmd.title, sizeof(cmd.title), "%s", title);
    }

    if (ioctl(ui->ctl_fd, WSIOC_ALLOC, &cmd) < 0) {
        if (errno == EPIPE) {
            errno = ENOENT;
        }
        return -1;
    }

    window->id = cmd.id;
    window->width = cmd.width;
    window->height = cmd.height;
    window->stride = cmd.stride;
    window_sync_framebuffer(window);

    if (!window_open_fds(window)) {
        return 0;
    }

    int saved = errno;
    ui_simple(ui, WSIOC_FREE, window->id, 0, 0, 0);
    window_reset(window, ui);
    errno = saved;
    return -1;
}

int window_from_env(ui_t *ui, window_t *window) {
    if (!ui || !window) {
        errno = EINVAL;
        return -1;
    }

    window_reset(window, ui);

    if (
        !parse_env_u32("WS_ID", &window->id) ||
        !parse_env_u32("WS_WIDTH", &window->width) ||
        !parse_env_u32("WS_HEIGHT", &window->height)
    ) {
        return -1;
    }

    if (!parse_env_u32("WS_STRIDE", &window->stride)) {
        if (errno != ENOENT) {
            return -1;
        }

        window->stride = window->width * 4;
    }

    window_sync_framebuffer(window);

    return window_open_fds(window);
}

int window_free(window_t *window) {
    if (!window || !window->ui) {
        errno = EINVAL;
        return -1;
    }

    int ret = ui_simple(window->ui, WSIOC_FREE, window->id, 0, 0, 0);
    window_close(window);
    return ret;
}

int window_set_title(window_t *window, const char *title) {
    if (!window || !window->ui || window->ui->ctl_fd < 0) {
        errno = EINVAL;
        return -1;
    }

    ws_cmd_t cmd = {0};
    cmd.id = window->id;

    if (title) {
        snprintf(cmd.title, sizeof(cmd.title), "%s", title);
    }

    return ioctl(window->ui->ctl_fd, WSIOC_SET_TITLE, &cmd);
}

void window_close(window_t *window) {
    if (!window) {
        return;
    }

    window_unmap_pixels(window);
    window_reset_runtime(window);
}

ssize_t
window_blit(window_t *window, const void *pixels, size_t len, size_t offset) {
    if (!window || window->fb_fd < 0 || !pixels) {
        errno = EINVAL;
        return -1;
    }

    return pwrite(window->fb_fd, pixels, len, (off_t)offset);
}

int window_init(window_t *window, u32 width, u32 height, const char *title) {
    if (!window) {
        errno = EINVAL;
        return -1;
    }

    memset(window, 0, sizeof(*window));
    window_reset_runtime(window);

    if (ui_open(&window->ui_local, 0)) {
        if (errno == ENOENT) {
            const char *msg = "error: window manager is not running\n";
            write(STDERR_FILENO, msg, strlen(msg));
        } else {
            const char *msg = "error: failed to open window system\n";
            write(STDERR_FILENO, msg, strlen(msg));
        }
        return -1;
    }

    window->ui = &window->ui_local;
    window->ui_owned = true;

    if (!window_from_env(window->ui, window)) {
        return 0;
    }

    int env_saved = errno;
    if (!window_alloc(window->ui, window, width, height, title)) {
        return 0;
    }

    int saved = errno;
    if (!saved) {
        saved = env_saved;
    }
    if (saved == ENOENT) {
        const char *msg = "error: window manager is not running\n";
        write(STDERR_FILENO, msg, strlen(msg));
    } else {
        const char *msg = "error: failed to create window\n";
        write(STDERR_FILENO, msg, strlen(msg));
    }

    ui_close(window->ui);
    memset(window, 0, sizeof(*window));
    window_reset_runtime(window);

    errno = saved;
    return -1;
}

void window_deinit(window_t *window) {
    if (!window) {
        return;
    }

    if (window->ui_owned && window->ui) {
        window_free(window);
        ui_close(window->ui);
    } else {
        window_close(window);
    }

    memset(window, 0, sizeof(*window));
    window_reset_runtime(window);
}

framebuffer_t *window_buffer(window_t *window) {
    if (!window || !window->width || !window->height) {
        errno = EINVAL;
        return NULL;
    }

    size_t pixels = (size_t)window->width * (size_t)window->height;
    if (window->height && pixels / window->height != window->width) {
        errno = EOVERFLOW;
        return NULL;
    }

    if (pixels > ((size_t)-1) / sizeof(pixel_t)) {
        errno = EOVERFLOW;
        return NULL;
    }

    if (window->pixels && window->pixels_capacity >= pixels) {
        window->pixels_count = pixels;
        window_sync_framebuffer(window);
        return &window->framebuffer;
    }

    size_t capacity = pixels;
    if (window->pixels_capacity) {
        capacity = window->pixels_capacity;

        while (capacity < pixels) {
            size_t grown = capacity * 2;

            if (grown <= capacity) {
                capacity = pixels;
                break;
            }
            capacity = grown;
        }
    }

    if (capacity > ((size_t)-1) / sizeof(pixel_t)) {
        errno = EOVERFLOW;
        return NULL;
    }

    size_t old_capacity = window->pixels_capacity;
    pixel_t *grown = realloc(window->pixels, capacity * sizeof(pixel_t));
    if (!grown) {
        return NULL;
    }

    if (capacity > old_capacity) {
        size_t zero_from = old_capacity;
        size_t zero_count = capacity - old_capacity;
        memset(grown + zero_from, 0, zero_count * sizeof(pixel_t));
    }

    window->pixels = grown;
    window->pixels_count = pixels;
    window->pixels_capacity = capacity;
    window_sync_framebuffer(window);

    return &window->framebuffer;
}

int window_flush(window_t *window) {
    if (!window || !window->pixels || !window->pixels_count) {
        errno = EINVAL;
        return -1;
    }

    size_t bytes = window->pixels_count * sizeof(pixel_t);
    ssize_t n = window_blit(window, window->pixels, bytes, 0);
    if (n == (ssize_t)bytes) {
        return 0;
    }

    if (n >= 0) {
        errno = EAGAIN;
    }

    return -1;
}

static int
window_flush_row(window_t *window, const u8 *row, size_t bytes, off_t offset) {
    if (!window || window->fb_fd < 0 || !row || !bytes) {
        errno = EINVAL;
        return -1;
    }

    size_t written = 0;

    while (written < bytes) {
        ssize_t n = pwrite(
            window->fb_fd,
            row + written,
            bytes - written,
            offset + (off_t)written
        );

        if (n < 0) {
            return -1;
        }

        if (!n) {
            errno = EAGAIN;
            return -1;
        }

        written += (size_t)n;
    }

    return 0;
}

static void _window_resize_preserve(
    pixel_t *pixels,
    u32 old_width,
    u32 old_height,
    u32 new_width,
    u32 new_height
) {
    if (!pixels || !old_width || !old_height || !new_width || !new_height) {
        return;
    }

    u32 copy_width = old_width < new_width ? old_width : new_width;
    u32 copy_height = old_height < new_height ? old_height : new_height;
    size_t copy_bytes = (size_t)copy_width * sizeof(pixel_t);
    size_t old_stride = (size_t)old_width * sizeof(pixel_t);
    size_t new_stride = (size_t)new_width * sizeof(pixel_t);

    if (new_stride > old_stride) {
        for (u32 row = copy_height; row > 0; row--) {
            size_t i = row - 1;
            u8 *dst = (u8 *)pixels + (i * new_stride);
            const u8 *src = (const u8 *)pixels + (i * old_stride);
            memmove(dst, src, copy_bytes);
        }
    } else {
        for (u32 row = 0; row < copy_height; row++) {
            u8 *dst = (u8 *)pixels + ((size_t)row * new_stride);
            const u8 *src = (const u8 *)pixels + ((size_t)row * old_stride);
            memmove(dst, src, copy_bytes);
        }
    }

    if (new_width > old_width) {
        for (u32 row = 0; row < copy_height; row++) {
            size_t row_off = (size_t)row * new_width;
            pixel_t fill = pixels[row_off + (old_width - 1)];
            for (u32 col = old_width; col < new_width; col++) {
                pixels[row_off + col] = fill;
            }
        }
    }

    if (new_height > old_height) {
        size_t src_row = (size_t)(old_height - 1) * new_width;
        for (u32 row = old_height; row < new_height; row++) {
            size_t row_off = (size_t)row * new_width;
            memcpy(
                pixels + row_off,
                pixels + src_row,
                (size_t)new_width * sizeof(pixel_t)
            );
        }
    }
}

static void
_window_apply_resize_default(window_t *window, const ws_input_event_t *event) {
    if (
        !window ||
        !event ||
        event->type != INPUT_EVENT_WINDOW_RESIZE ||
        !event->width ||
        !event->height
    ) {
        return;
    }

    u32 new_width = event->width;
    u32 new_height = event->height;
    u32 new_stride =
        event->stride ? event->stride : event->width * sizeof(pixel_t);

    if (
        window->width == new_width &&
        window->height == new_height &&
        window->stride == new_stride
    ) {
        return;
    }

    u32 old_width = window->width;
    u32 old_height = window->height;
    u32 old_stride = window->stride;

    bool had_pixels =
        window->pixels && window->pixels_count && old_width && old_height;

    window->width = new_width;
    window->height = new_height;
    window->stride = new_stride;

    framebuffer_t *fb = window_buffer(window);
    if (!fb || !fb->pixels) {
        window->width = old_width;
        window->height = old_height;
        window->stride = old_stride;
        window_sync_framebuffer(window);
        return;
    }

    if (had_pixels) {
        _window_resize_preserve(
            fb->pixels, old_width, old_height, new_width, new_height
        );
    } else if (window->pixels_count) {
        memset(fb->pixels, 0, window->pixels_count * sizeof(pixel_t));
    }

    window_sync_framebuffer(window);
}

ssize_t
window_events(window_t *window, ws_input_event_t *events, size_t count) {
    if (!window || window->ev_fd < 0 || !events || !count) {
        errno = EINVAL;
        return -1;
    }

    ssize_t n = read(window->ev_fd, events, count * sizeof(*events));
    if (!n) {
        errno = ENOENT;
        return -1;
    }

    if (n < 0) {
        return -1;
    }

    if ((size_t)n % sizeof(*events)) {
        errno = EIO;
        return -1;
    }

    size_t event_count = (size_t)n / sizeof(*events);
    for (size_t i = 0; i < event_count; i++) {
        if (events[i].type == INPUT_EVENT_WINDOW_RESIZE) {
            _window_apply_resize_default(window, &events[i]);
        }
    }

    return n;
}

int window_flush_rect(window_t *window, u32 x, u32 y, u32 width, u32 height) {
    if (!window || !window->pixels || !window->pixels_count) {
        errno = EINVAL;
        return -1;
    }

    if (x >= window->width || y >= window->height || !width || !height) {
        return 0;
    }

    u32 clip_w = width;
    u32 clip_h = height;

    if (x + clip_w > window->width) {
        clip_w = window->width - x;
    }

    if (y + clip_h > window->height) {
        clip_h = window->height - y;
    }

    if (x == 0 && clip_w == window->width && window->stride == window->width * sizeof(pixel_t)) {
        const u8 *src =
            (const u8 *)(window->pixels + (size_t)y * window->width);

        size_t total = (size_t)clip_h * (size_t)window->width * sizeof(pixel_t);
        off_t dst_off = (off_t)((size_t)y * window->stride);

        return window_flush_row(window, src, total, dst_off);
    }

    size_t row_bytes = (size_t)clip_w * sizeof(pixel_t);

    for (u32 row = 0; row < clip_h; row++) {
        const u8 *src = 
            (const u8 *)(window->pixels + ((size_t)y + row) * window->width + x);

        off_t dst_off =
            (off_t)(((size_t)y + row) * window->stride + (size_t)x * sizeof(pixel_t));

        if (window_flush_row(window, src, row_bytes, dst_off) < 0) {
            return -1;
        }
    }

    return 0;
}

int window_wait_event(
    window_t *window,
    ws_input_event_t *event,
    int timeout_ms
) {
    if (!window || window->ev_fd < 0 || !event) {
        errno = EINVAL;
        return -1;
    }

    struct pollfd pfd = {
        .fd = window->ev_fd,
        .events = POLLIN,
        .revents = 0,
    };

    int ready = poll(&pfd, 1, timeout_ms);
    if (ready <= 0) {
        return ready;
    }

    if (pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) {
        errno = ENOENT;
        return -1;
    }

    if (!(pfd.revents & POLLIN)) {
        return 0;
    }

    ssize_t n = window_events(window, event, 1);
    if (n == (ssize_t)sizeof(*event)) {
        return 1;
    }

    if (n > 0) {
        errno = EIO;
    }

    return -1;
}
