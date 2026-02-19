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
#include <string.h>
#include <sys/ioctl.h>
#include <ui.h>
#include <unistd.h>

#include "wm.h"
#include "wm_cursor.h"
#include "wm_rect.h"

extern char **environ;

#define WM_POLL_DRAG_MS    16
#define WM_POLL_FRAME_MS   16
#define WM_CURSOR_WIDTH    16
#define WM_CURSOR_HEIGHT   16

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

static bool _rect_clip_to_fb(wm_rect_t *rect, const fb_info_t *fb_info) {
    if (!rect || !wm_rect_valid(rect) || !fb_info) {
        return false;
    }

    i32 x0 = rect->x;
    i32 y0 = rect->y;
    i32 x1 = rect->x + rect->width;
    i32 y1 = rect->y + rect->height;

    if (x0 < 0) {
        x0 = 0;
    }
    if (y0 < 0) {
        y0 = 0;
    }
    if (x1 > (i32)fb_info->width) {
        x1 = (i32)fb_info->width;
    }
    if (y1 > (i32)fb_info->height) {
        y1 = (i32)fb_info->height;
    }

    if (x0 >= x1 || y0 >= y1) {
        memset(rect, 0, sizeof(*rect));
        return false;
    }

    rect->x = x0;
    rect->y = y0;
    rect->width = x1 - x0;
    rect->height = y1 - y0;

    return true;
}

static wm_rect_t _cursor_rect(i32 x, i32 y) {
    wm_rect_t rect = {
        .x = x,
        .y = y,
        .width = WM_CURSOR_WIDTH,
        .height = WM_CURSOR_HEIGHT,
    };

    return rect;
}

static bool _title_rect(const wm_window_t *window, wm_rect_t *rect) {
    if (!window || !rect) {
        return false;
    }

    rect->x = window->x;
    rect->y = window->y;
    rect->width = (i32)window->width;
    rect->height = TITLE_H;
    return wm_rect_valid(rect);
}

static void _focus_window(ui_t *ui, wm_runtime_t *rt, wm_window_t *window, wm_rect_t *damage) {
    if (!ui || !rt || !window || !damage) {
        return;
    }

    wm_window_t *prev = NULL;
    if (rt->focused_id >= 0) {
        prev = wm_window_by_id((u32)rt->focused_id);
    }

    u32 old_z = window->z;
    wm_set_focus(ui, window, &rt->z_counter);
    rt->focused_id = (int)window->id;

    wm_rect_t title = {0};
    if (prev && prev != window && _title_rect(prev, &title)) {
        wm_rect_union(damage, &title);
    }

    if (_title_rect(window, &title)) {
        wm_rect_union(damage, &title);
    }

    if (window->z != old_z) {
        wm_collect_raise_damage(window, old_z, damage);
    }
}

static int _present_damage(int fb_fd, const fb_info_t *fb_info, const u32 *frame, wm_rect_t *damage) {
    if (!fb_info || !frame || !damage || !wm_rect_valid(damage)) {
        return 0;
    }

    wm_rect_t clipped = *damage;
    if (!_rect_clip_to_fb(&clipped, fb_info)) {
        return 0;
    }

    fb_present_rect_t req = {
        .frame = frame,
        .x = (u32)clipped.x,
        .y = (u32)clipped.y,
        .width = (u32)clipped.width,
        .height = (u32)clipped.height,
    };

    int ret = ioctl(fb_fd, FBIOPRESENT_RECT, &req);

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

static int _handle_ws_events(ui_t *ui, wm_runtime_t *rt, wm_rect_t *damage) {
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

            wm_rect_t event_damage = {0};
            if (wm_handle_ws_event(&events[i], &event_damage)) {
                wm_rect_union(damage, &event_damage);
            }

            if (events[i].type == WS_EVT_WINDOW_NEW) {
                wm_window_t *win = wm_window_by_id(events[i].id);
                if (win) {
                    _focus_window(ui, rt, win, damage);
                } else {
                    rt->focused_id = -1;
                }
            }

            if (focus_closed) {
                wm_window_t *top = wm_top_window();
                if (top) {
                    _focus_window(ui, rt, top, damage);
                } else {
                    rt->focused_id = -1;
                }
            }
        }
    }
}

static bool _handle_mouse_move(
    ui_t *ui,
    wm_runtime_t *rt,
    const fb_info_t *fb_info,
    const input_event_t *event,
    wm_rect_t *damage
) {
    i32 prev_x = rt->mouse_x;
    i32 prev_y = rt->mouse_y;

    rt->mouse_x += event->dx;
    rt->mouse_y += event->dy;
    _clamp_mouse(rt, fb_info);

    bool changed = (rt->mouse_x != prev_x) || (rt->mouse_y != prev_y);

    if (changed) {
        wm_rect_t old_cursor = _cursor_rect(prev_x, prev_y);
        wm_rect_t new_cursor = _cursor_rect(rt->mouse_x, rt->mouse_y);
        wm_rect_union(damage, &old_cursor);
        wm_rect_union(damage, &new_cursor);
    }

    if (rt->drag_id < 0 || !(rt->mouse_btn_state & MOUSE_LEFT_CLICK)) {
        return changed;
    }

    wm_window_t *window = wm_window_by_id((u32)rt->drag_id);
    if (!window) {
        return changed;
    }

    wm_rect_t old_rect = {0};
    wm_window_bounds_rect(window, &old_rect);

    i32 next_x = rt->mouse_x - rt->drag_dx;
    i32 next_y = rt->mouse_y - rt->drag_dy;

    if (window->x == next_x && window->y == next_y) {
        return changed;
    }

    window->x = next_x;
    window->y = next_y;

    ui_mgr_move(ui, window->id, window->x, window->y);

    wm_rect_t new_rect = {0};
    wm_window_bounds_rect(window, &new_rect);
    wm_rect_union(damage, &old_rect);
    wm_rect_union(damage, &new_rect);

    return true;
}

static bool _handle_mouse_button(ui_t *ui, wm_runtime_t *rt, const input_event_t *event, wm_rect_t *damage) {
    u32 prev = rt->mouse_btn_state;
    rt->mouse_btn_state = event->buttons;
    bool changed = prev != rt->mouse_btn_state;

    if (!(prev & MOUSE_LEFT_CLICK) && (rt->mouse_btn_state & MOUSE_LEFT_CLICK)) {
        wm_window_t *window = wm_top_window_at(rt->mouse_x, rt->mouse_y);

        if (window) {
            _focus_window(ui, rt, window, damage);

            if (wm_point_in_close(window, rt->mouse_x, rt->mouse_y)) {
                wm_rect_t close_damage = {0};
                wm_window_bounds_rect(window, &close_damage);
                wm_rect_union(damage, &close_damage);

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
    bool *changed,
    wm_rect_t *damage
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
                if (_handle_mouse_move(ui, rt, fb_info, event, damage)) {
                    *changed = true;
                }
            } else if (event->type == INPUT_EVENT_MOUSE_BUTTON) {
                if (_handle_mouse_button(ui, rt, event, damage)) {
                    wm_rect_t cursor = _cursor_rect(rt->mouse_x, rt->mouse_y);
                    wm_rect_union(damage, &cursor);
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
    (void)frame_bytes;

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

    struct pollfd pfds[3] = {
        {.fd = ui->keyboard_fd, .events = POLLIN, .revents = 0},
        {.fd = ui->mouse_fd, .events = POLLIN, .revents = 0},
        {.fd = ui->mgr_fd, .events = POLLIN, .revents = 0},
    };

    wm_rect_t damage = {
        .x = 0,
        .y = 0,
        .width = (i32)fb_info->width,
        .height = (i32)fb_info->height,
    };

    for (;;) {
        if (*exit_requested) {
            return;
        }

        bool has_windows = wm_top_window() != NULL;
        bool needs_redraw = wm_rect_valid(&damage);
        int timeout_ms = -1;

        if (rt.drag_id >= 0) {
            timeout_ms = WM_POLL_DRAG_MS;
        } else if (needs_redraw) {
            timeout_ms = WM_POLL_FRAME_MS;
        }

        int pr = poll(pfds, 3, timeout_ms);

        if (pr < 0) {
            if (errno == EINTR) {
                continue;
            }

            return;
        }

        if (pfds[2].revents & POLLIN) {
            if (_handle_ws_events(ui, &rt, &damage) < 0) {
                return;
            }
        }

        if ((pfds[0].revents & POLLIN) || (pfds[1].revents & POLLIN)) {
            bool changed = false;
            if (_handle_input_events(ui, &rt, fb_info, exit_requested, &changed, &damage) < 0) {
                return;
            }

            if (changed && !wm_rect_valid(&damage)) {
                wm_rect_t cursor = _cursor_rect(rt.mouse_x, rt.mouse_y);
                wm_rect_union(&damage, &cursor);
            }
        }

        if (!wm_rect_valid(&damage)) {
            if (!pr && has_windows) {
                continue;
            }

            continue;
        }

        wm_render_damage(frame_store, fb_info->width, fb_info->height, &damage);
        wm_cursor_draw(frame_store, fb_info->width, fb_info->height, rt.mouse_x, rt.mouse_y);

        int present = _present_damage(fb_fd, fb_info, frame_store, &damage);

        if (present > 0) {
            continue;
        }

        if (present < 0) {
            return;
        }

        memset(&damage, 0, sizeof(damage));
    }
}
