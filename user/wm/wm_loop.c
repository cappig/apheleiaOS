#include "wm_loop.h"

#include <base/macros.h>
#include <draw.h>
#include <errno.h>
#include <gui/fb.h>
#include <input/kbd.h>
#include <input/mouse.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <ui.h>
#include <unistd.h>

#include "wm.h"

extern char** environ;

#define WM_POLL_DRAG_MS   16
#define WM_POLL_FRAME_MS  16
#define WM_POLL_IDLE_MS   33

typedef struct {
    i32 mouse_x;
    i32 mouse_y;
    u32 mouse_btn_state;
    u32 z_counter;
    int drag_id;
    i32 drag_dx;
    i32 drag_dy;
    wm_window_t* focused;
    bool term_hotkey_down;
} wm_runtime_t;

static int _present_frame(int fb_fd, const fb_info_t* fb_info, const u32* frame, size_t frame_bytes) {
    if (!fb_info || !frame)
        return -1;

    size_t packed_row_bytes = (size_t)fb_info->width * 4;

    if ((size_t)fb_info->pitch == packed_row_bytes) {
        size_t written = 0;
        const u8* src = (const u8*)frame;

        while (written < frame_bytes) {
            ssize_t n = pwrite(fb_fd, src + written, frame_bytes - written, (off_t)written);

            if (n < 0) {
                if (errno == EINTR)
                    continue;

                if (errno == EAGAIN)
                    return 1;

                return -1;
            }

            if (!n)
                return -1;

            written += (size_t)n;
        }

        return 0;
    }

    size_t height = (size_t)fb_info->height;
    const u8* src = (const u8*)frame;

    for (size_t row = 0; row < height; row++) {
        size_t written = 0;
        off_t row_off = (off_t)(row * (size_t)fb_info->pitch);

        const u8* row_src = src + row * packed_row_bytes;

        while (written < packed_row_bytes) {
            ssize_t n = pwrite(
                fb_fd,
                row_src + written,
                packed_row_bytes - written,
                row_off + (off_t)written
            );

            if (n < 0) {
                if (errno == EINTR)
                    continue;

                if (errno == EAGAIN)
                    return 1;

                return -1;
            }

            if (!n)
                return -1;

            written += (size_t)n;
        }
    }

    return 0;
}

static void _spawn_term(void) {
    pid_t pid = fork();
    if (pid < 0)
        return;

    if (!pid) {
        char* argv[] = {"/sbin/term", NULL};
        char* empty_env[] = {NULL};
        char* const* envp = environ ? environ : empty_env;
        execve("/sbin/term", argv, envp);
        _exit(127);
    }
}

static void _clamp_mouse(wm_runtime_t* rt, const fb_info_t* fb_info) {
    if (rt->mouse_x < 0)
        rt->mouse_x = 0;

    if (rt->mouse_y < 0)
        rt->mouse_y = 0;

    if (rt->mouse_x >= (i32)fb_info->width)
        rt->mouse_x = (i32)fb_info->width - 1;

    if (rt->mouse_y >= (i32)fb_info->height)
        rt->mouse_y = (i32)fb_info->height - 1;
}

static int _handle_ws_events(ui_t* ui, wm_runtime_t* rt) {
    ws_event_t events[8];

    for (;;) {
        ssize_t n = ui_mgr_events(ui, events, ARRAY_LEN(events));

        if (n < 0) {
            if (errno == EAGAIN)
                return 0;

            return -1;
        }

        if (!n)
            return 0;

        size_t count = (size_t)n / sizeof(ws_event_t);

        for (size_t i = 0; i < count; i++) {
            bool focus_closed =
                rt->focused && events[i].type == WS_EVT_WINDOW_CLOSED && rt->focused->id == events[i].id;

            wm_handle_ws_event(&events[i]);

            if (events[i].type == WS_EVT_WINDOW_NEW) {
                rt->focused = wm_window_by_id(events[i].id);
                wm_set_focus(ui, rt->focused, &rt->z_counter);
            }

            if (focus_closed) {
                rt->focused = wm_top_window();

                if (rt->focused)
                    wm_set_focus(ui, rt->focused, &rt->z_counter);
            }
        }
    }
}

static bool _handle_mouse_move(ui_t* ui, wm_runtime_t* rt, const fb_info_t* fb_info, const input_event_t* event) {
    i32 prev_x = rt->mouse_x;
    i32 prev_y = rt->mouse_y;

    rt->mouse_x += event->dx;
    rt->mouse_y += event->dy;
    _clamp_mouse(rt, fb_info);

    bool changed = (rt->mouse_x != prev_x) || (rt->mouse_y != prev_y);

    if (rt->drag_id < 0 || !(rt->mouse_btn_state & MOUSE_LEFT_CLICK))
        return changed;

    wm_window_t* window = wm_window_by_id((u32)rt->drag_id);
    if (!window || !window->used)
        return changed;

    i32 next_x = rt->mouse_x - rt->drag_dx;
    i32 next_y = rt->mouse_y - rt->drag_dy;

    if (window->x == next_x && window->y == next_y)
        return changed;

    window->x = next_x;
    window->y = next_y;

    ui_mgr_move(ui, window->id, window->x, window->y);
    return true;
}

static bool _handle_mouse_button(ui_t* ui, wm_runtime_t* rt, const input_event_t* event) {
    u32 prev = rt->mouse_btn_state;
    rt->mouse_btn_state = event->buttons;
    bool changed = prev != rt->mouse_btn_state;

    if (!(prev & MOUSE_LEFT_CLICK) && (rt->mouse_btn_state & MOUSE_LEFT_CLICK)) {
        wm_window_t* window = wm_top_window_at(rt->mouse_x, rt->mouse_y);

        if (window) {
            wm_set_focus(ui, window, &rt->z_counter);
            rt->focused = window;

            if (wm_point_in_close(window, rt->mouse_x, rt->mouse_y)) {
                ui_mgr_close(ui, window->id);
                rt->focused = NULL;
                rt->drag_id = -1;
                changed = true;
            } else if (wm_point_in_title(window, rt->mouse_x, rt->mouse_y)) {
                rt->drag_id = (int)window->id;
                rt->drag_dx = rt->mouse_x - window->x;
                rt->drag_dy = rt->mouse_y - window->y;
                changed = true;
            } else {
                changed = true;
            }
        }
    }

    if (!(rt->mouse_btn_state & MOUSE_LEFT_CLICK))
        rt->drag_id = -1;

    return changed;
}

static bool _handle_key(ui_t* ui, wm_runtime_t* rt, input_event_t* event, volatile sig_atomic_t* exit_requested) {
    bool ctrl = (event->modifiers & INPUT_MOD_CTRL) != 0;
    bool alt = (event->modifiers & INPUT_MOD_ALT) != 0;

    if (event->action && ctrl && alt && (event->keycode == KBD_BACKSPACE || event->keycode == KBD_Q)) {
        *exit_requested = 1;
        return true;
    }

    if (event->keycode == KBD_T && ctrl) {
        if (!event->action) {
            rt->term_hotkey_down = false;
        } else if (!rt->term_hotkey_down) {
            rt->term_hotkey_down = true;
            _spawn_term();
            return true;
        }
    }

    if (event->action && ctrl && event->keycode == KBD_W && rt->focused) {
        ui_mgr_close(ui, rt->focused->id);
        rt->focused = NULL;
        return true;
    }

    return false;
}

static int _handle_input_events(
    ui_t* ui, wm_runtime_t* rt, const fb_info_t* fb_info, volatile sig_atomic_t* exit_requested, bool* changed
) {
    input_event_t events[16];

    for (;;) {
        ssize_t n = ui_input(ui, events, ARRAY_LEN(events));

        if (n < 0) {
            if (errno == EAGAIN)
                return 0;

            return -1;
        }

        if (!n)
            return 0;

        size_t count = (size_t)n / sizeof(input_event_t);

        for (size_t i = 0; i < count; i++) {
            input_event_t* event = &events[i];

            if (event->type == INPUT_EVENT_MOUSE_MOVE) {
                if (_handle_mouse_move(ui, rt, fb_info, event))
                    *changed = true;
            } else if (event->type == INPUT_EVENT_MOUSE_BUTTON) {
                if (_handle_mouse_button(ui, rt, event))
                    *changed = true;
            } else if (event->type == INPUT_EVENT_KEY) {
                *changed = true;

                if (_handle_key(ui, rt, event, exit_requested))
                    continue;
            }

            if (rt->focused)
                ui_mgr_send(ui, rt->focused->id, event);
        }
    }
}

void wm_loop(ui_t* ui, int fb_fd, const fb_info_t* fb_info, u32* frame_store, size_t frame_bytes, volatile sig_atomic_t* exit_requested) {
    wm_runtime_t rt = {
        .mouse_x = (i32)(fb_info->width / 2),
        .mouse_y = (i32)(fb_info->height / 2),
        .mouse_btn_state = 0,
        .z_counter = 100,
        .drag_id = -1,
        .drag_dx = 0,
        .drag_dy = 0,
        .focused = NULL,
        .term_hotkey_down = false,
    };

    struct pollfd pfds[2] = {
        {.fd = ui->input_fd, .events = POLLIN, .revents = 0},
        {.fd = ui->ctl_fd, .events = POLLIN, .revents = 0},
    };

    bool needs_redraw = true;

    for (;;) {
        if (*exit_requested)
            return;

        bool has_windows = wm_top_window() != NULL;
        int timeout_ms = -1;

        if (rt.drag_id >= 0)
            timeout_ms = WM_POLL_DRAG_MS;
        else if (needs_redraw)
            timeout_ms = WM_POLL_FRAME_MS;
        else if (has_windows)
            timeout_ms = WM_POLL_IDLE_MS;

        int pr = poll(pfds, 2, timeout_ms);

        if (pr < 0) {
            if (errno == EINTR)
                continue;

            return;
        }

        if (!pr && has_windows)
            needs_redraw = true;

        if (pfds[1].revents & POLLIN) {
            if (_handle_ws_events(ui, &rt) < 0)
                return;

            needs_redraw = true;
        }

        if (pfds[0].revents & POLLIN) {
            bool changed = false;
            if (_handle_input_events(ui, &rt, fb_info, exit_requested, &changed) < 0)
                return;

            if (changed)
                needs_redraw = true;
        }

        if (!needs_redraw)
            continue;

        wm_render_frame(frame_store, fb_info->width, fb_info->height);
        draw_fill_rect(
            frame_store,
            fb_info->width,
            fb_info->height,
            rt.mouse_x - 2,
            rt.mouse_y - 2,
            5,
            5,
            0x00ffffffU
        );

        int present = _present_frame(fb_fd, fb_info, frame_store, frame_bytes);

        if (present > 0)
            continue;

        if (present < 0)
            return;

        needs_redraw = false;
    }
}
