#include "wm_loop.h"

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

static void spawn_term(void) {
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

static void clamp_mouse(wm_runtime_t* rt, const fb_info_t* fb_info) {
    if (rt->mouse_x < 0)
        rt->mouse_x = 0;

    if (rt->mouse_y < 0)
        rt->mouse_y = 0;

    if (rt->mouse_x >= (i32)fb_info->width)
        rt->mouse_x = (i32)fb_info->width - 1;

    if (rt->mouse_y >= (i32)fb_info->height)
        rt->mouse_y = (i32)fb_info->height - 1;
}

static int handle_ws_events(ui_t* ui, wm_runtime_t* rt) {
    ws_event_t events[8];

    for (;;) {
        ssize_t n = ui_mgr_events(ui, events, sizeof(events) / sizeof(events[0]));

        if (n < 0) {
            if (errno == EAGAIN)
                return 0;

            return -1;
        }

        if (!n)
            return 0;

        size_t count = (size_t)n / sizeof(ws_event_t);

        for (size_t i = 0; i < count; i++) {
            bool focus_closed = rt->focused && events[i].type == WS_EVT_WINDOW_CLOSED && rt->focused->id == events[i].id;

            wm_handle_ws_event(&events[i]);

            if (events[i].type == WS_EVT_WINDOW_NEW && !rt->focused) {
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

static void handle_mouse_move(ui_t* ui, wm_runtime_t* rt, const fb_info_t* fb_info, const input_event_t* event) {
    rt->mouse_x += event->dx;
    rt->mouse_y += event->dy;
    clamp_mouse(rt, fb_info);

    if (rt->drag_id < 0 || !(rt->mouse_btn_state & MOUSE_LEFT_CLICK))
        return;

    wm_window_t* window = wm_window_by_id((u32)rt->drag_id);
    if (!window || !window->used)
        return;

    window->x = rt->mouse_x - rt->drag_dx;
    window->y = rt->mouse_y - rt->drag_dy;

    ui_mgr_move(ui, window->id, window->x, window->y);
}

static void handle_mouse_button(ui_t* ui, wm_runtime_t* rt, const input_event_t* event) {
    u32 prev = rt->mouse_btn_state;
    rt->mouse_btn_state = event->buttons;

    if (!(prev & MOUSE_LEFT_CLICK) && (rt->mouse_btn_state & MOUSE_LEFT_CLICK)) {
        wm_window_t* window = wm_top_window_at(rt->mouse_x, rt->mouse_y);

        if (window) {
            wm_set_focus(ui, window, &rt->z_counter);
            rt->focused = window;

            if (wm_point_in_close(window, rt->mouse_x, rt->mouse_y)) {
                ui_mgr_close(ui, window->id);
                rt->focused = NULL;
                rt->drag_id = -1;
            } else if (wm_point_in_title(window, rt->mouse_x, rt->mouse_y)) {
                rt->drag_id = (int)window->id;
                rt->drag_dx = rt->mouse_x - window->x;
                rt->drag_dy = rt->mouse_y - window->y;
            }
        }
    }

    if (!(rt->mouse_btn_state & MOUSE_LEFT_CLICK))
        rt->drag_id = -1;
}

static bool handle_key(ui_t* ui, wm_runtime_t* rt, input_event_t* event, volatile sig_atomic_t* exit_requested) {
    bool ctrl = (event->modifiers & INPUT_MOD_CTRL) != 0;
    bool alt = (event->modifiers & INPUT_MOD_ALT) != 0;

    if (event->action && ctrl && alt && (event->keycode == KBD_BACKSPACE || event->keycode == KBD_Q)) {
        *exit_requested = 1;
        return true;
    }

    if (event->keycode == KBD_T) {
        if (!event->action)
            rt->term_hotkey_down = false;

        else if ((event->modifiers & INPUT_MOD_CTRL) && !rt->term_hotkey_down) {
            rt->term_hotkey_down = true;
            spawn_term();
            return true;
        }
    }

    if (event->action && event->keycode == KBD_W && (event->modifiers & INPUT_MOD_CTRL) && rt->focused) {
        ui_mgr_close(ui, rt->focused->id);
        rt->focused = NULL;
        return true;
    }

    return false;
}

static int handle_input_events(ui_t* ui, wm_runtime_t* rt, const fb_info_t* fb_info, volatile sig_atomic_t* exit_requested) {
    input_event_t events[16];

    for (;;) {
        ssize_t n = ui_input(ui, events, sizeof(events) / sizeof(events[0]));

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
                handle_mouse_move(ui, rt, fb_info, event);
            } else if (event->type == INPUT_EVENT_MOUSE_BUTTON) {
                handle_mouse_button(ui, rt, event);
            } else if (event->type == INPUT_EVENT_KEY) {
                if (handle_key(ui, rt, event, exit_requested))
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

    for (;;) {
        if (*exit_requested)
            return;

        int pr = poll(pfds, 2, 16);

        if (pr < 0) {
            if (errno == EINTR)
                continue;

            return;
        }

        if ((pfds[1].revents & POLLIN) && handle_ws_events(ui, &rt) < 0)
            return;

        if ((pfds[0].revents & POLLIN) && handle_input_events(ui, &rt, fb_info, exit_requested) < 0)
            return;

        wm_render_frame(frame_store, fb_info->width, fb_info->height);
        draw_fill_rect(frame_store, fb_info->width, fb_info->height, rt.mouse_x - 2, rt.mouse_y - 2, 5, 5, 0x00ffffffU);

        if (pwrite(fb_fd, frame_store, frame_bytes, 0) < 0) {
            if (errno == EAGAIN)
                continue;

            return;
        }
    }
}
