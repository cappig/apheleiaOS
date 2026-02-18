#include "wm_loop.h"

#include <base/macros.h>
#include <errno.h>
#include <gui/fb.h>
#include <input/kbd.h>
#include <input/mouse.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/ioctl.h>
#include <ui.h>
#include <unistd.h>

#include "wm.h"
#include "wm_cursor.h"

extern char **environ;

#define WM_POLL_DRAG_MS  16
#define WM_POLL_FRAME_MS 16

typedef struct {
    i32 mouse_x;
    i32 mouse_y;
    u32 mouse_btn_state;
    u32 z_counter;
    int drag_id;
    i32 drag_dx;
    i32 drag_dy;
    int focused_id;
    bool term_hotkey_down;
} wm_runtime_t;


static int
_present_frame(int fb_fd, const fb_info_t *fb_info, const u32 *frame, size_t frame_bytes) {
    (void)frame_bytes;

    if (!fb_info || !frame) {
        return -1;
    }

    int ret = ioctl(fb_fd, FBIOPRESENT, frame);

    if (ret < 0) {
        if (errno == EAGAIN) {
            return 1;
        }

        return -1;
    }

    return 0;
}

static void _spawn_term(void) {
    pid_t pid = fork();
    if (pid < 0) {
        return;
    }

    if (!pid) {
        char *argv[] = {"/bin/term", NULL};
        char *empty_env[] = {NULL};
        char *const *envp = environ ? environ : empty_env;
        execve("/bin/term", argv, envp);
        _exit(127);
    }
}

static void _clamp_mouse(wm_runtime_t *rt, const fb_info_t *fb_info) {
    if (rt->mouse_x < 0) {
        rt->mouse_x = 0;
    }

    if (rt->mouse_y < 0) {
        rt->mouse_y = 0;
    }

    if (rt->mouse_x >= (i32)fb_info->width) {
        rt->mouse_x = (i32)fb_info->width - 1;
    }

    if (rt->mouse_y >= (i32)fb_info->height) {
        rt->mouse_y = (i32)fb_info->height - 1;
    }
}

static int _handle_ws_events(ui_t *ui, wm_runtime_t *rt) {
    ws_event_t events[8];

    for (;;) {
        ssize_t n = ui_mgr_events(ui, events, ARRAY_LEN(events));

        if (n < 0) {
            if (errno == EAGAIN) {
                return 0;
            }

            return -1;
        }

        if (!n) {
            return 0;
        }

        size_t count = (size_t)n / sizeof(ws_event_t);

        for (size_t i = 0; i < count; i++) {
            bool focus_closed = rt->focused_id >= 0 && events[i].type == WS_EVT_WINDOW_CLOSED &&
                                (u32)rt->focused_id == events[i].id;

            wm_handle_ws_event(&events[i]);

            if (events[i].type == WS_EVT_WINDOW_NEW) {
                wm_window_t *win = wm_window_by_id(events[i].id);
                rt->focused_id = win ? (int)win->id : -1;
                wm_set_focus(ui, win, &rt->z_counter);
            }

            if (focus_closed) {
                wm_window_t *top = wm_top_window();
                rt->focused_id = top ? (int)top->id : -1;

                if (top) {
                    wm_set_focus(ui, top, &rt->z_counter);
                }
            }
        }
    }
}

static bool _handle_mouse_move(
    ui_t *ui,
    wm_runtime_t *rt,
    const fb_info_t *fb_info,
    const input_event_t *event
) {
    i32 prev_x = rt->mouse_x;
    i32 prev_y = rt->mouse_y;

    rt->mouse_x += event->dx;
    rt->mouse_y += event->dy;
    _clamp_mouse(rt, fb_info);

    bool changed = (rt->mouse_x != prev_x) || (rt->mouse_y != prev_y);

    if (rt->drag_id < 0 || !(rt->mouse_btn_state & MOUSE_LEFT_CLICK)) {
        return changed;
    }

    wm_window_t *window = wm_window_by_id((u32)rt->drag_id);
    if (!window) {
        return changed;
    }

    i32 next_x = rt->mouse_x - rt->drag_dx;
    i32 next_y = rt->mouse_y - rt->drag_dy;

    if (window->x == next_x && window->y == next_y) {
        return changed;
    }

    window->x = next_x;
    window->y = next_y;

    ui_mgr_move(ui, window->id, window->x, window->y);
    return true;
}

static bool _handle_mouse_button(ui_t *ui, wm_runtime_t *rt, const input_event_t *event) {
    u32 prev = rt->mouse_btn_state;
    rt->mouse_btn_state = event->buttons;
    bool changed = prev != rt->mouse_btn_state;

    if (!(prev & MOUSE_LEFT_CLICK) && (rt->mouse_btn_state & MOUSE_LEFT_CLICK)) {
        wm_window_t *window = wm_top_window_at(rt->mouse_x, rt->mouse_y);

        if (window) {
            wm_set_focus(ui, window, &rt->z_counter);
            rt->focused_id = (int)window->id;

            if (wm_point_in_close(window, rt->mouse_x, rt->mouse_y)) {
                ui_mgr_close(ui, window->id);
                rt->focused_id = -1;
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

    if (!(rt->mouse_btn_state & MOUSE_LEFT_CLICK)) {
        rt->drag_id = -1;
    }

    return changed;
}

static bool _handle_key(
    ui_t *ui,
    wm_runtime_t *rt,
    input_event_t *event,
    volatile sig_atomic_t *exit_requested
) {
    bool ctrl = (event->modifiers & INPUT_MOD_CTRL) != 0;
    bool alt = (event->modifiers & INPUT_MOD_ALT) != 0;

    if (event->action && ctrl && alt &&
        (event->keycode == KBD_BACKSPACE || event->keycode == KBD_Q)) {
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

    if (event->action && ctrl && event->keycode == KBD_W && rt->focused_id >= 0) {
        ui_mgr_close(ui, (u32)rt->focused_id);
        rt->focused_id = -1;
        return true;
    }

    return false;
}

static int _handle_input_events(
    ui_t *ui,
    wm_runtime_t *rt,
    const fb_info_t *fb_info,
    volatile sig_atomic_t *exit_requested,
    bool *changed
) {
    input_event_t events[16];

    for (;;) {
        ssize_t n = ui_input(ui, events, ARRAY_LEN(events));

        if (n < 0) {
            if (errno == EAGAIN) {
                return 0;
            }

            return -1;
        }

        if (!n) {
            return 0;
        }

        size_t count = (size_t)n / sizeof(input_event_t);

        for (size_t i = 0; i < count; i++) {
            input_event_t *event = &events[i];

            if (event->type == INPUT_EVENT_MOUSE_MOVE) {
                if (_handle_mouse_move(ui, rt, fb_info, event)) {
                    *changed = true;
                }
            } else if (event->type == INPUT_EVENT_MOUSE_BUTTON) {
                if (_handle_mouse_button(ui, rt, event)) {
                    *changed = true;
                }
            } else if (event->type == INPUT_EVENT_KEY) {
                *changed = true;

                if (_handle_key(ui, rt, event, exit_requested)) {
                    continue;
                }
            }

            if (rt->focused_id >= 0) {
                ui_mgr_send(ui, (u32)rt->focused_id, event);
            }
        }
    }
}

void wm_loop(
    ui_t *ui,
    int fb_fd,
    const fb_info_t *fb_info,
    u32 *frame_store,
    size_t frame_bytes,
    volatile sig_atomic_t *exit_requested
) {
    wm_runtime_t rt = {
        .mouse_x = (i32)(fb_info->width / 2),
        .mouse_y = (i32)(fb_info->height / 2),
        .mouse_btn_state = 0,
        .z_counter = 100,
        .drag_id = -1,
        .drag_dx = 0,
        .drag_dy = 0,
        .focused_id = -1,
        .term_hotkey_down = false,
    };

    struct pollfd pfds[2] = {
        {.fd = ui->input_fd, .events = POLLIN, .revents = 0},
        {.fd = ui->ctl_fd, .events = POLLIN, .revents = 0},
    };

    bool needs_redraw = true;

    for (;;) {
        if (*exit_requested) {
            return;
        }

        bool has_windows = wm_top_window() != NULL;
        int timeout_ms = -1;

        if (rt.drag_id >= 0) {
            timeout_ms = WM_POLL_DRAG_MS;
        } else if (needs_redraw) {
            timeout_ms = WM_POLL_FRAME_MS;
        }

        int pr = poll(pfds, 2, timeout_ms);

        if (pr < 0) {
            if (errno == EINTR) {
                continue;
            }

            return;
        }

        if (pfds[1].revents & POLLIN) {
            int ws_rc = _handle_ws_events(ui, &rt);
            if (ws_rc < 0) {
                return;
            }

            // ctl_fd is pollable for ws events AND window fb dirty
            needs_redraw = true;
        }

        // ctl_fd polls readable when window content is dirty too
        if (!pr && !needs_redraw && has_windows) {
            continue;
        }

        if (pfds[0].revents & POLLIN) {
            bool changed = false;
            if (_handle_input_events(ui, &rt, fb_info, exit_requested, &changed) < 0) {
                return;
            }

            if (changed) {
                needs_redraw = true;
            }
        }

        if (!needs_redraw) {
            continue;
        }

        wm_render_frame(frame_store, fb_info->width, fb_info->height);
        wm_cursor_draw(frame_store, fb_info->width, fb_info->height, rt.mouse_x, rt.mouse_y);

        int present = _present_frame(fb_fd, fb_info, frame_store, frame_bytes);

        if (present > 0) {
            continue;
        }

        if (present < 0) {
            return;
        }

        ui_mgr_clear_dirty(ui);
        needs_redraw = false;
    }
}
