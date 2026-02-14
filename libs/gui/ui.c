#include "ui.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

extern char** environ;

static const char* env_lookup(const char* key) {
    if (!key || !key[0] || !environ)
        return NULL;

    size_t key_len = strlen(key);
    if (!key_len)
        return NULL;

    for (char** env = environ; *env; env++) {
        if (strncmp(*env, key, key_len))
            continue;

        if ((*env)[key_len] != '=')
            continue;

        return *env + key_len + 1;
    }

    return NULL;
}

static bool parse_env_u32(const char* key, u32* out) {
    if (!key || !out) {
        errno = EINVAL;
        return false;
    }

    const char* value = env_lookup(key);
    if (!value || !value[0]) {
        errno = ENOENT;
        return false;
    }

    char* end = NULL;
    long parsed = strtol(value, &end, 10);
    if (!end || *end || parsed < 0) {
        errno = EINVAL;
        return false;
    }

    *out = (u32)parsed;
    return true;
}

static void close_fd(int* fd) {
    if (!fd || *fd < 0)
        return;

    close(*fd);
    *fd = -1;
}

static int ui_simple(ui_t* ui, u32 op, u32 id, i32 x, i32 y, u32 flags) {
    ws_req_t req = {0};
    ws_resp_t resp = {0};

    req.op = op;
    req.id = id;
    req.x = x;
    req.y = y;
    req.flags = flags;

    return ui_rpc(ui, &req, &resp);
}

static void window_reset_runtime(window_t* window) {
    if (!window)
        return;

    close_fd(&window->fb_fd);
    close_fd(&window->ev_fd);
    window->pixels = NULL;
    window->pixels_count = 0;
}

static void window_unmap_pixels(window_t* window) {
    if (!window || !window->pixels || !window->pixels_count)
        return;

    size_t bytes = window->pixels_count * sizeof(u32);
    munmap(window->pixels, bytes);

    window->pixels = NULL;
    window->pixels_count = 0;
}

static void window_reset(window_t* window, ui_t* ui) {
    if (!window)
        return;

    window_unmap_pixels(window);
    window_reset_runtime(window);

    window->ui = ui;
    window->id = 0;
    window->width = 0;
    window->height = 0;
    window->stride = 0;
}

static int window_open_fds(window_t* window) {
    if (!window) {
        errno = EINVAL;
        return -1;
    }

    char fb_path[64];
    char ev_path[64];

    snprintf(fb_path, sizeof(fb_path), "/dev/ws/%u/fb", window->id);
    snprintf(ev_path, sizeof(ev_path), "/dev/ws/%u/ev", window->id);

    window->fb_fd = open(fb_path, O_RDWR, 0);
    if (window->fb_fd < 0)
        return -1;

    window->ev_fd = open(ev_path, O_RDONLY, 0);
    if (window->ev_fd >= 0)
        return 0;

    int saved = errno;
    close_fd(&window->fb_fd);
    errno = saved;
    return -1;
}

int ui_open(ui_t* ui, u32 flags) {
    if (!ui) {
        errno = EINVAL;
        return -1;
    }

    ui->ctl_fd = -1;
    ui->input_fd = -1;

    ui->ctl_fd = open("/dev/wsctl", O_RDWR | O_NONBLOCK, 0);
    if (ui->ctl_fd < 0)
        return -1;

    if (!(flags & UI_OPEN_INPUT))
        return 0;

    ui->input_fd = open("/dev/input", O_RDONLY | O_NONBLOCK, 0);
    if (ui->input_fd >= 0)
        return 0;

    int saved = errno;
    ui_close(ui);
    errno = saved;
    return -1;
}

void ui_close(ui_t* ui) {
    if (!ui)
        return;

    close_fd(&ui->input_fd);
    close_fd(&ui->ctl_fd);
}

int ui_rpc(ui_t* ui, const ws_req_t* req, ws_resp_t* resp) {
    if (!ui || ui->ctl_fd < 0 || !req || !resp) {
        errno = EINVAL;
        return -1;
    }

    if (write(ui->ctl_fd, req, sizeof(*req)) != (ssize_t)sizeof(*req))
        return -1;

    ssize_t n = read(ui->ctl_fd, resp, sizeof(*resp));
    if (n != (ssize_t)sizeof(*resp)) {
        if (n >= 0)
            errno = EIO;

        return -1;
    }

    if (resp->status < 0) {
        errno = -resp->status;
        return -1;
    }

    return 0;
}

ssize_t ui_input(ui_t* ui, input_event_t* events, size_t count) {
    if (!ui || ui->input_fd < 0 || !events || !count) {
        errno = EINVAL;
        return -1;
    }

    return read(ui->input_fd, events, count * sizeof(*events));
}

int ui_mgr_claim(ui_t* ui) {
    return ui_simple(ui, WS_OP_CLAIM_MANAGER, 0, 0, 0, 0);
}

int ui_mgr_release(ui_t* ui) {
    return ui_simple(ui, WS_OP_RELEASE_MANAGER, 0, 0, 0, 0);
}

ssize_t ui_mgr_events(ui_t* ui, ws_event_t* events, size_t count) {
    if (!ui || ui->ctl_fd < 0 || !events || !count) {
        errno = EINVAL;
        return -1;
    }

    return read(ui->ctl_fd, events, count * sizeof(*events));
}

int ui_mgr_focus(ui_t* ui, u32 id) {
    return ui_simple(ui, WS_OP_SET_FOCUS, id, 0, 0, 0);
}

int ui_mgr_move(ui_t* ui, u32 id, i32 x, i32 y) {
    return ui_simple(ui, WS_OP_SET_POS, id, x, y, 0);
}

int ui_mgr_raise(ui_t* ui, u32 id, u32 z) {
    return ui_simple(ui, WS_OP_SET_Z, id, 0, 0, z);
}

int ui_mgr_close(ui_t* ui, u32 id) {
    return ui_simple(ui, WS_OP_CLOSE, id, 0, 0, 0);
}

int ui_mgr_send(ui_t* ui, u32 id, const input_event_t* event) {
    if (!ui || !event) {
        errno = EINVAL;
        return -1;
    }

    ws_req_t req = {0};
    ws_resp_t resp = {0};

    req.op = WS_OP_SEND_INPUT;
    req.id = id;
    req.input.timestamp_ms = event->timestamp_ms;
    req.input.type = event->type;
    req.input.source = event->source;
    req.input.keycode = event->keycode;
    req.input.action = event->action;
    req.input.buttons = event->buttons;
    req.input.modifiers = event->modifiers;
    req.input.dx = event->dx;
    req.input.dy = event->dy;
    req.input.wheel = event->wheel;

    return ui_rpc(ui, &req, &resp);
}

int window_alloc(ui_t* ui, window_t* window, u32 width, u32 height, const char* title) {
    if (!ui || !window) {
        errno = EINVAL;
        return -1;
    }

    window_reset(window, ui);

    ws_req_t req = {0};
    ws_resp_t resp = {0};

    req.op = WS_OP_ALLOC;
    req.width = width;
    req.height = height;
    if (title)
        snprintf(req.title, sizeof(req.title), "%s", title);

    if (ui_rpc(ui, &req, &resp))
        return -1;

    window->id = resp.id;
    window->width = resp.width;
    window->height = resp.height;
    window->stride = resp.stride;

    if (!window_open_fds(window))
        return 0;

    int saved = errno;
    ui_simple(ui, WS_OP_FREE, window->id, 0, 0, 0);
    window_reset(window, ui);
    errno = saved;
    return -1;
}

int window_from_env(ui_t* ui, window_t* window) {
    if (!ui || !window) {
        errno = EINVAL;
        return -1;
    }

    window_reset(window, ui);

    if (!parse_env_u32("WS_ID", &window->id) || !parse_env_u32("WS_WIDTH", &window->width) ||
        !parse_env_u32("WS_HEIGHT", &window->height))
        return -1;

    if (!parse_env_u32("WS_STRIDE", &window->stride)) {
        if (errno != ENOENT)
            return -1;

        window->stride = window->width * 4;
    }

    return window_open_fds(window);
}

int window_free(window_t* window) {
    if (!window || !window->ui) {
        errno = EINVAL;
        return -1;
    }

    int ret = ui_simple(window->ui, WS_OP_FREE, window->id, 0, 0, 0);
    window_close(window);
    return ret;
}

void window_close(window_t* window) {
    if (!window)
        return;

    window_unmap_pixels(window);
    window_reset_runtime(window);
}

ssize_t window_blit(window_t* window, const void* pixels, size_t len, size_t offset) {
    if (!window || window->fb_fd < 0 || !pixels) {
        errno = EINVAL;
        return -1;
    }

    return pwrite(window->fb_fd, pixels, len, (off_t)offset);
}

ssize_t window_events(window_t* window, ws_input_event_t* events, size_t count) {
    if (!window || window->ev_fd < 0 || !events || !count) {
        errno = EINVAL;
        return -1;
    }

    return read(window->ev_fd, events, count * sizeof(*events));
}

int window_init(window_t* window, u32 width, u32 height, const char* title) {
    if (!window) {
        errno = EINVAL;
        return -1;
    }

    memset(window, 0, sizeof(*window));
    window_reset_runtime(window);

    if (ui_open(&window->ui_local, 0))
        return -1;

    window->ui = &window->ui_local;
    window->ui_owned = true;

    if (!window_from_env(window->ui, window))
        return 0;

    if (errno == ENOENT && !window_alloc(window->ui, window, width, height, title))
        return 0;

    int saved = errno;
    ui_close(window->ui);
    memset(window, 0, sizeof(*window));
    window_reset_runtime(window);
    errno = saved;
    return -1;
}

void window_deinit(window_t* window) {
    if (!window)
        return;

    if (window->ui_owned && window->ui) {
        window_free(window);
        ui_close(window->ui);
    } else {
        window_close(window);
    }

    memset(window, 0, sizeof(*window));
    window_reset_runtime(window);
}

u32* window_buffer(window_t* window) {
    if (!window || !window->width || !window->height) {
        errno = EINVAL;
        return NULL;
    }

    size_t pixels = (size_t)window->width * (size_t)window->height;
    if (window->height && pixels / window->height != window->width) {
        errno = EOVERFLOW;
        return NULL;
    }

    if (pixels > ((size_t)-1) / sizeof(u32)) {
        errno = EOVERFLOW;
        return NULL;
    }

    if (window->pixels && window->pixels_count == pixels)
        return window->pixels;

    window_unmap_pixels(window);

    size_t bytes = pixels * sizeof(u32);
    void* map = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (map == MAP_FAILED)
        return NULL;

    window->pixels = map;
    window->pixels_count = pixels;
    return window->pixels;
}

int window_flush(window_t* window) {
    if (!window || !window->pixels || !window->pixels_count) {
        errno = EINVAL;
        return -1;
    }

    size_t bytes = window->pixels_count * sizeof(u32);
    ssize_t n = window_blit(window, window->pixels, bytes, 0);
    if (n == (ssize_t)bytes)
        return 0;

    if (n >= 0)
        errno = EIO;

    return -1;
}

int window_wait_event(window_t* window, ws_input_event_t* event, int timeout_ms) {
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
    if (ready <= 0)
        return ready;

    if (!(pfd.revents & POLLIN))
        return 0;

    ssize_t n = window_events(window, event, 1);
    if (n == (ssize_t)sizeof(*event))
        return 1;

    if (!n)
        return 0;

    if (n > 0)
        errno = EIO;

    return -1;
}
