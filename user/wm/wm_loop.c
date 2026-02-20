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
#include <stdlib.h>
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
#define WM_CURSOR_CENTER_HOT_X (WM_CURSOR_WIDTH / 2)
#define WM_CURSOR_CENTER_HOT_Y (WM_CURSOR_HEIGHT / 2)
#define WM_RESIZE_EDGE_MARGIN   6
#define WM_RESIZE_CORNER_MARGIN 12
#define WM_MIN_CLIENT_W    160
#define WM_MIN_CLIENT_H    96
#define WM_WS_EVENT_BATCH  32
#define WM_INPUT_EVENT_BATCH 32

typedef enum {
    WM_DRAG_NONE = 0,
    WM_DRAG_MOVE = 1,
    WM_DRAG_RESIZE = 2,
} wm_drag_mode_t;

enum wm_resize_edge {
    WM_RESIZE_LEFT = 1u << 0,
    WM_RESIZE_RIGHT = 1u << 1,
    WM_RESIZE_TOP = 1u << 2,
    WM_RESIZE_BOTTOM = 1u << 3,
};

typedef struct {
    i32 mouse_x;
    i32 mouse_y;
    u32 mouse_btn_state;
    u32 z_counter;
    int drag_id;
    wm_drag_mode_t drag_mode;
    u32 drag_edges;
    i32 drag_mouse_start_x;
    i32 drag_mouse_start_y;
    i32 drag_window_start_x;
    i32 drag_window_start_y;
    u32 drag_window_start_width;
    u32 drag_window_start_height;
    i32 drag_synced_x;
    i32 drag_synced_y;
    u32 drag_synced_width;
    u32 drag_synced_height;
    bool drag_synced_valid;
    bool drag_release_sync_pending;
    int focused_id;
    bool term_hotkey_down;
    wm_cursor_kind_t cursor_kind;
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
        .x = x - WM_CURSOR_CENTER_HOT_X,
        .y = y - WM_CURSOR_CENTER_HOT_Y,
        .width = WM_CURSOR_WIDTH + WM_CURSOR_CENTER_HOT_X,
        .height = WM_CURSOR_HEIGHT + WM_CURSOR_CENTER_HOT_Y,
    };

    return rect;
}

static void _window_damage_union_bounds(const wm_window_t *window, wm_rect_t *damage) {
    if (!window || !damage) {
        return;
    }

    wm_rect_t rect = {0};
    if (wm_window_bounds_rect(window, &rect)) {
        wm_rect_union(damage, &rect);
    }
}

static void _window_mark_full_dirty(wm_window_t *window) {
    if (!window || !window->width || !window->height) {
        return;
    }

    window->surface_dirty = true;
    window->dirty_x = 0;
    window->dirty_y = 0;
    window->dirty_width = window->width;
    window->dirty_height = window->height;
}

static void _window_clear_dirty(wm_window_t *window) {
    if (!window) {
        return;
    }

    window->surface_dirty = false;
    window->dirty_x = 0;
    window->dirty_y = 0;
    window->dirty_width = 0;
    window->dirty_height = 0;
}

static bool _apply_window_geometry(
    wm_window_t *window,
    wm_rect_t *damage,
    i32 x,
    i32 y,
    u32 width,
    u32 height
) {
    if (!window || !damage) {
        return false;
    }

    if (window->x == x && window->y == y && window->width == width && window->height == height) {
        return false;
    }

    _window_damage_union_bounds(window, damage);
    window->x = x;
    window->y = y;
    window->width = width;
    window->height = height;
    _window_damage_union_bounds(window, damage);
    return true;
}

static bool _is_mouse_event(u32 type) {
    return type == INPUT_EVENT_MOUSE_MOVE || type == INPUT_EVENT_MOUSE_BUTTON ||
           type == INPUT_EVENT_MOUSE_WHEEL;
}

static void _mark_consumed(bool *consumed) {
    if (consumed) {
        *consumed = true;
    }
}

typedef struct {
    bool valid;
    u32 target_id;
    input_event_t event;
} wm_forward_batch_t;

static void _flush_forward_batch(ui_t *ui, wm_forward_batch_t *batch) {
    if (!ui || !batch || !batch->valid) {
        return;
    }

    ui_mgr_send(ui, batch->target_id, &batch->event);
    batch->valid = false;
    batch->target_id = 0;
    memset(&batch->event, 0, sizeof(batch->event));
}

static void _queue_forward_move(
    ui_t *ui,
    wm_forward_batch_t *batch,
    u32 target_id,
    const input_event_t *event
) {
    if (!ui || !batch || !event || event->type != INPUT_EVENT_MOUSE_MOVE) {
        return;
    }

    if (!batch->valid || batch->target_id != target_id || batch->event.type != INPUT_EVENT_MOUSE_MOVE) {
        _flush_forward_batch(ui, batch);
        batch->valid = true;
        batch->target_id = target_id;
        batch->event = *event;
        return;
    }

    batch->event.timestamp_ms = event->timestamp_ms;
    batch->event.source = event->source;
    batch->event.buttons = event->buttons;
    batch->event.modifiers = event->modifiers;
    batch->event.dx += event->dx;
    batch->event.dy += event->dy;
}

static int _sync_drag_resize(
    ui_t *ui,
    wm_runtime_t *rt,
    wm_window_t *window,
    wm_rect_t *damage,
    bool refresh_surface
) {
    if (!ui || !rt || !window) {
        return -EINVAL;
    }

    if (ui_mgr_resize(ui, window->id, window->width, window->height) < 0) {
        return errno ? -errno : -EIO;
    }

    bool needs_move =
        !rt->drag_synced_valid || rt->drag_synced_x != window->x || rt->drag_synced_y != window->y;
    if (needs_move) {
        ui_mgr_move(ui, window->id, window->x, window->y);
    }

    window->fb_width = window->width;
    window->fb_height = window->height;

    rt->drag_synced_x = window->x;
    rt->drag_synced_y = window->y;
    rt->drag_synced_width = window->width;
    rt->drag_synced_height = window->height;
    rt->drag_synced_valid = true;

    if (refresh_surface) {
        _window_mark_full_dirty(window);
        _window_damage_union_bounds(window, damage);
    }

    return 0;
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

static bool _center_window_on_screen(
    ui_t *ui,
    const fb_info_t *fb_info,
    wm_window_t *window,
    wm_rect_t *damage
) {
    if (!ui || !fb_info || !window || !damage) {
        return false;
    }

    i32 frame_w = (i32)fb_info->width;
    i32 frame_h = (i32)fb_info->height;
    i32 win_w = (i32)window->width;
    i32 win_h = TITLE_H + (i32)window->height;

    i32 x = 0;
    i32 y = 0;

    if (win_w < frame_w) {
        x = (frame_w - win_w) / 2;
    }
    if (win_h < frame_h) {
        y = (frame_h - win_h) / 2;
    }

    if (!_apply_window_geometry(window, damage, x, y, window->width, window->height)) {
        return false;
    }

    ui_mgr_move(ui, window->id, window->x, window->y);
    return true;
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

static int _present_damage(int fb_fd, const fb_info_t *fb_info, const pixel_t *frame, wm_rect_t *damage) {
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

static void _clear_drag(wm_runtime_t *rt) {
    if (!rt) {
        return;
    }

    rt->drag_id = -1;
    rt->drag_mode = WM_DRAG_NONE;
    rt->drag_edges = 0;
    rt->drag_synced_valid = false;
    rt->drag_release_sync_pending = false;
}

static void _begin_drag(wm_runtime_t *rt, const wm_window_t *window) {
    if (!rt || !window) {
        return;
    }

    rt->drag_id = (int)window->id;
    rt->drag_mouse_start_x = rt->mouse_x;
    rt->drag_mouse_start_y = rt->mouse_y;
    rt->drag_window_start_x = window->x;
    rt->drag_window_start_y = window->y;
    rt->drag_window_start_width = window->width;
    rt->drag_window_start_height = window->height;
    rt->drag_synced_x = window->x;
    rt->drag_synced_y = window->y;
    rt->drag_synced_width = window->width;
    rt->drag_synced_height = window->height;
    rt->drag_synced_valid = true;
    rt->drag_release_sync_pending = false;
}

static u32 _resize_hit_edges(const wm_window_t *window, i32 px, i32 py) {
    if (!window) {
        return 0;
    }

    if (wm_point_in_close(window, px, py)) {
        return 0;
    }

    wm_rect_t bounds = {0};
    if (!wm_window_bounds_rect(window, &bounds)) {
        return 0;
    }

    i32 left = bounds.x;
    i32 top = bounds.y;
    i32 right = bounds.x + bounds.width - 1;
    i32 bottom = bounds.y + bounds.height - 1;

    if (px < left || px > right || py < top || py > bottom) {
        return 0;
    }

    bool near_left_edge = (px - left) < WM_RESIZE_EDGE_MARGIN;
    bool near_right_edge = (right - px) < WM_RESIZE_EDGE_MARGIN;
    bool near_bottom_edge = (bottom - py) < WM_RESIZE_EDGE_MARGIN;

    bool near_left_corner = (px - left) < WM_RESIZE_CORNER_MARGIN;
    bool near_right_corner = (right - px) < WM_RESIZE_CORNER_MARGIN;
    bool near_top_corner = (py - top) < WM_RESIZE_CORNER_MARGIN;
    bool near_bottom_corner = (bottom - py) < WM_RESIZE_CORNER_MARGIN;

    if (near_left_corner && near_top_corner) {
        return WM_RESIZE_LEFT | WM_RESIZE_TOP;
    }
    if (near_right_corner && near_top_corner) {
        return 0;
    }
    if (near_left_corner && near_bottom_corner) {
        return WM_RESIZE_LEFT | WM_RESIZE_BOTTOM;
    }
    if (near_right_corner && near_bottom_corner) {
        return WM_RESIZE_RIGHT | WM_RESIZE_BOTTOM;
    }

    u32 edges = 0;
    if (near_left_edge) {
        edges |= WM_RESIZE_LEFT;
    }
    if (near_right_edge) {
        edges |= WM_RESIZE_RIGHT;
    }
    if (near_bottom_edge) {
        edges |= WM_RESIZE_BOTTOM;
    }

    return edges;
}

static wm_cursor_kind_t _cursor_kind_for_edges(u32 edges) {
    bool left = (edges & WM_RESIZE_LEFT) != 0;
    bool right = (edges & WM_RESIZE_RIGHT) != 0;
    bool top = (edges & WM_RESIZE_TOP) != 0;
    bool bottom = (edges & WM_RESIZE_BOTTOM) != 0;

    if (top && left) {
        return WM_CURSOR_RESIZE_NW;
    }
    if (bottom && left) {
        return WM_CURSOR_RESIZE_SW;
    }
    if (bottom && right) {
        return WM_CURSOR_RESIZE_SE;
    }
    if (left || right) {
        return WM_CURSOR_RESIZE_EW;
    }
    if (top || bottom) {
        return WM_CURSOR_RESIZE_NS;
    }

    return WM_CURSOR_NORMAL;
}

static wm_cursor_kind_t _cursor_kind_for_state(const wm_runtime_t *rt) {
    if (!rt) {
        return WM_CURSOR_NORMAL;
    }

    if (rt->drag_mode == WM_DRAG_RESIZE && rt->drag_id >= 0) {
        return _cursor_kind_for_edges(rt->drag_edges);
    }

    wm_window_t *window = wm_top_window_at(rt->mouse_x, rt->mouse_y);
    if (!window) {
        return WM_CURSOR_NORMAL;
    }

    return _cursor_kind_for_edges(_resize_hit_edges(window, rt->mouse_x, rt->mouse_y));
}

typedef struct {
    i32 x;
    i32 y;
    u32 width;
    u32 height;
} wm_resize_geometry_t;

typedef struct {
    i32 pos;
    i32 size;
} wm_resize_axis_t;

static void _resize_axis_apply_delta(wm_resize_axis_t *axis, i32 delta, bool has_min_edge, bool has_max_edge) {
    if (!axis) {
        return;
    }

    if (has_min_edge) {
        axis->pos += delta;
        axis->size -= delta;
    }
    if (has_max_edge) {
        axis->size += delta;
    }
}

static void _resize_axis_clamp(
    wm_resize_axis_t *axis,
    i32 min_size,
    i32 frame_size,
    bool has_min_edge,
    bool has_max_edge
) {
    if (!axis || frame_size <= 0) {
        return;
    }

    if (axis->size < min_size) {
        if (has_min_edge && !has_max_edge) {
            axis->pos -= (min_size - axis->size);
        }
        axis->size = min_size;
    }

    if (axis->pos < 0) {
        if (has_min_edge && !has_max_edge) {
            axis->size += axis->pos;
        }
        axis->pos = 0;
    }

    if (axis->size > frame_size) {
        axis->size = frame_size;
        axis->pos = 0;
    }

    i32 overflow = axis->pos + axis->size - frame_size;
    if (overflow > 0) {
        if (has_max_edge && !has_min_edge) {
            axis->size -= overflow;
        } else {
            axis->pos -= overflow;
        }
    }

    if (axis->size < min_size) {
        axis->size = min_size;
        if (axis->size > frame_size) {
            axis->size = frame_size;
        }
        axis->pos = frame_size - axis->size;
    }

    if (axis->pos < 0) {
        axis->pos = 0;
    }
}

static bool _compute_resize_geometry(
    const wm_runtime_t *rt,
    const fb_info_t *fb_info,
    i32 delta_x,
    i32 delta_y,
    wm_resize_geometry_t *out
) {
    if (!rt || !fb_info || !out) {
        return false;
    }

    i32 frame_w = (i32)fb_info->width;
    i32 frame_h = (i32)fb_info->height;
    i32 min_w = WM_MIN_CLIENT_W;
    i32 min_total_h = TITLE_H + WM_MIN_CLIENT_H;

    bool left = (rt->drag_edges & WM_RESIZE_LEFT) != 0;
    bool right = (rt->drag_edges & WM_RESIZE_RIGHT) != 0;
    bool top = (rt->drag_edges & WM_RESIZE_TOP) != 0;
    bool bottom = (rt->drag_edges & WM_RESIZE_BOTTOM) != 0;

    wm_resize_axis_t axis_x = {
        .pos = rt->drag_window_start_x,
        .size = (i32)rt->drag_window_start_width,
    };
    wm_resize_axis_t axis_y = {
        .pos = rt->drag_window_start_y,
        .size = TITLE_H + (i32)rt->drag_window_start_height,
    };

    _resize_axis_apply_delta(&axis_x, delta_x, left, right);
    _resize_axis_apply_delta(&axis_y, delta_y, top, bottom);

    _resize_axis_clamp(&axis_x, min_w, frame_w, left, right);
    _resize_axis_clamp(&axis_y, min_total_h, frame_h, top, bottom);

    if (axis_x.size <= 0 || axis_y.size <= TITLE_H) {
        return false;
    }

    u32 next_width = (u32)axis_x.size;
    u32 next_height = (u32)(axis_y.size - TITLE_H);
    if (!next_width || !next_height) {
        return false;
    }

    out->x = axis_x.pos;
    out->y = axis_y.pos;
    out->width = next_width;
    out->height = next_height;
    return true;
}

static bool _apply_drag(ui_t *ui, wm_runtime_t *rt, const fb_info_t *fb_info, wm_rect_t *damage) {
    if (!ui || !rt || !fb_info || !damage) {
        return false;
    }

    if (rt->drag_id < 0 || !(rt->mouse_btn_state & MOUSE_LEFT_CLICK)) {
        return false;
    }

    wm_window_t *window = wm_window_by_id((u32)rt->drag_id);
    if (!window) {
        _clear_drag(rt);
        return false;
    }

    i32 delta_x = rt->mouse_x - rt->drag_mouse_start_x;
    i32 delta_y = rt->mouse_y - rt->drag_mouse_start_y;

    i32 next_x = window->x;
    i32 next_y = window->y;
    u32 next_width = window->width;
    u32 next_height = window->height;

    if (rt->drag_mode == WM_DRAG_MOVE) {
        next_x = rt->drag_window_start_x + delta_x;
        next_y = rt->drag_window_start_y + delta_y;
        if (!_apply_window_geometry(window, damage, next_x, next_y, next_width, next_height)) {
            return false;
        }
        ui_mgr_move(ui, window->id, window->x, window->y);
        return true;
    }

    if (rt->drag_mode != WM_DRAG_RESIZE) {
        return false;
    }

    wm_resize_geometry_t geometry = {0};
    if (!_compute_resize_geometry(rt, fb_info, delta_x, delta_y, &geometry)) {
        return false;
    }

    next_x = geometry.x;
    next_y = geometry.y;
    next_width = geometry.width;
    next_height = geometry.height;
    return _apply_window_geometry(window, damage, next_x, next_y, next_width, next_height);
}

static int _handle_ws_events(ui_t *ui, wm_runtime_t *rt, const fb_info_t *fb_info, wm_rect_t *damage) {
    ws_event_t events[WM_WS_EVENT_BATCH];

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
            if (events[i].type == WS_EVT_SCREEN_ACTIVE) {
                wm_rect_t full = {
                    .x = 0,
                    .y = 0,
                    .width = (i32)fb_info->width,
                    .height = (i32)fb_info->height,
                };
                wm_rect_union(damage, &full);
                continue;
            }

            bool suppress_drag_dirty = rt->drag_id >= 0 &&
                                       (rt->drag_mode == WM_DRAG_RESIZE ||
                                        rt->drag_release_sync_pending) &&
                                       events[i].type == WS_EVT_WINDOW_DIRTY &&
                                       events[i].id == (u32)rt->drag_id;

            if (suppress_drag_dirty) {
                continue;
            }

            bool focus_closed = rt->focused_id >= 0 && events[i].type == WS_EVT_WINDOW_CLOSED &&
                                (u32)rt->focused_id == events[i].id;

            wm_rect_t event_damage = {0};
            if (wm_handle_ws_event(&events[i], &event_damage)) {
                wm_rect_union(damage, &event_damage);
            }

            if (events[i].type == WS_EVT_WINDOW_NEW) {
                wm_window_t *win = wm_window_by_id(events[i].id);
                if (win) {
                    _center_window_on_screen(ui, fb_info, win, damage);
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
    (void)ui;
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

    return changed;
}

static bool _handle_mouse_button(
    ui_t *ui,
    wm_runtime_t *rt,
    const input_event_t *event,
    wm_rect_t *damage,
    bool *consumed
) {
    if (consumed) {
        *consumed = false;
    }

    u32 prev = rt->mouse_btn_state;
    rt->mouse_btn_state = event->buttons;
    bool changed = prev != rt->mouse_btn_state;

    if (!(prev & MOUSE_LEFT_CLICK) && (rt->mouse_btn_state & MOUSE_LEFT_CLICK)) {
        _clear_drag(rt);
        wm_window_t *window = wm_top_window_at(rt->mouse_x, rt->mouse_y);

        if (window) {
            _focus_window(ui, rt, window, damage);

            if (wm_point_in_close(window, rt->mouse_x, rt->mouse_y)) {
                _mark_consumed(consumed);
                wm_rect_t close_damage = {0};
                wm_window_bounds_rect(window, &close_damage);
                wm_rect_union(damage, &close_damage);

                ui_mgr_close(ui, window->id);
                rt->focused_id = -1;
                _clear_drag(rt);
                changed = true;
            } else {
                u32 edges = _resize_hit_edges(window, rt->mouse_x, rt->mouse_y);

                _begin_drag(rt, window);

                if (edges) {
                    rt->drag_mode = WM_DRAG_RESIZE;
                    rt->drag_edges = edges;
                    _window_clear_dirty(window);
                    _mark_consumed(consumed);
                    changed = true;
                } else if (wm_point_in_title(window, rt->mouse_x, rt->mouse_y)) {
                    rt->drag_mode = WM_DRAG_MOVE;
                    rt->drag_edges = 0;
                    _mark_consumed(consumed);
                    changed = true;
                } else {
                    _clear_drag(rt);
                    changed = true;
                }
            }
        }
    }

    if (!(rt->mouse_btn_state & MOUSE_LEFT_CLICK)) {
        if ((prev & MOUSE_LEFT_CLICK) && rt->drag_mode != WM_DRAG_NONE) {
            _mark_consumed(consumed);
        }

        if ((prev & MOUSE_LEFT_CLICK) && rt->drag_mode == WM_DRAG_RESIZE && rt->drag_id >= 0) {
            wm_window_t *window = wm_window_by_id((u32)rt->drag_id);
            if (window) {
                int sync = _sync_drag_resize(ui, rt, window, damage, true);
                if (sync == -EAGAIN) {
                    rt->drag_release_sync_pending = true;
                    return changed;
                }
            }
        }

        _clear_drag(rt);
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
    input_event_t events[WM_INPUT_EVENT_BATCH];
    wm_forward_batch_t forward_batch = {0};

    for (;;) {
        ssize_t n = ui_input(ui, events, ARRAY_LEN(events));

        if (n <= 0) {
            _flush_forward_batch(ui, &forward_batch);
            if (!n || errno == EAGAIN) {
                return 0;
            }
            return -1;
        }

        size_t count = (size_t)n / sizeof(input_event_t);

        for (size_t i = 0; i < count; i++) {
            input_event_t *event = &events[i];
            bool consumed_mouse = false;
            bool key_handled = false;

            if (event->type != INPUT_EVENT_MOUSE_MOVE) {
                _flush_forward_batch(ui, &forward_batch);
            }

            if (event->type == INPUT_EVENT_MOUSE_MOVE) {
                if (_handle_mouse_move(ui, rt, fb_info, event, damage)) {
                    *changed = true;
                }
                if (rt->drag_mode != WM_DRAG_NONE) {
                    consumed_mouse = true;
                }
            } else if (event->type == INPUT_EVENT_MOUSE_BUTTON) {
                if (_handle_mouse_button(ui, rt, event, damage, &consumed_mouse)) {
                    wm_rect_t cursor = _cursor_rect(rt->mouse_x, rt->mouse_y);
                    wm_rect_union(damage, &cursor);
                    *changed = true;
                }
                if (rt->drag_mode != WM_DRAG_NONE || rt->drag_release_sync_pending) {
                    consumed_mouse = true;
                }
            } else if (event->type == INPUT_EVENT_KEY) {
                *changed = true;

                if (_handle_key(ui, rt, event, exit_requested)) {
                    key_handled = true;
                }
            }

            if (rt->focused_id < 0) {
                _flush_forward_batch(ui, &forward_batch);
                continue;
            }

            bool should_forward = true;
            if (_is_mouse_event(event->type) && consumed_mouse) {
                should_forward = false;
            }
            if (key_handled) {
                should_forward = false;
            }

            if (!should_forward) {
                if (event->type == INPUT_EVENT_MOUSE_MOVE) {
                    _flush_forward_batch(ui, &forward_batch);
                }
                continue;
            }

            if (event->type == INPUT_EVENT_MOUSE_MOVE) {
                _queue_forward_move(ui, &forward_batch, (u32)rt->focused_id, event);
                continue;
            }

            ui_mgr_send(ui, (u32)rt->focused_id, event);
        }
    }
}

void wm_loop(
    ui_t *ui,
    int fb_fd,
    const fb_info_t *fb_info,
    pixel_t *frame_store,
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
        .drag_mode = WM_DRAG_NONE,
        .drag_edges = 0,
        .drag_mouse_start_x = 0,
        .drag_mouse_start_y = 0,
        .drag_window_start_x = 0,
        .drag_window_start_y = 0,
        .drag_window_start_width = 0,
        .drag_window_start_height = 0,
        .drag_synced_x = 0,
        .drag_synced_y = 0,
        .drag_synced_width = 0,
        .drag_synced_height = 0,
        .drag_synced_valid = false,
        .drag_release_sync_pending = false,
        .focused_id = -1,
        .term_hotkey_down = false,
        .cursor_kind = WM_CURSOR_NORMAL,
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
            if (_handle_ws_events(ui, &rt, fb_info, &damage) < 0) {
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

        if (rt.drag_id >= 0 && (rt.mouse_btn_state & MOUSE_LEFT_CLICK)) {
            _apply_drag(ui, &rt, fb_info, &damage);
        }

        if (rt.drag_release_sync_pending && rt.drag_id >= 0 && !(rt.mouse_btn_state & MOUSE_LEFT_CLICK)) {
            wm_window_t *window = wm_window_by_id((u32)rt.drag_id);
            if (!window) {
                _clear_drag(&rt);
            } else {
                int sync = _sync_drag_resize(ui, &rt, window, &damage, true);
                if (!sync) {
                    _clear_drag(&rt);
                } else if (sync != -EAGAIN) {
                    _clear_drag(&rt);
                }
            }
        }

        wm_cursor_kind_t cursor_kind = _cursor_kind_for_state(&rt);
        if (cursor_kind != rt.cursor_kind) {
            wm_rect_t cursor = _cursor_rect(rt.mouse_x, rt.mouse_y);
            wm_rect_union(&damage, &cursor);
            rt.cursor_kind = cursor_kind;
        }

        if (!wm_rect_valid(&damage)) {
            if (!pr && has_windows) {
                continue;
            }

            continue;
        }

        wm_render_damage(frame_store, fb_info->width, fb_info->height, &damage);
        wm_cursor_draw_kind(
            frame_store,
            fb_info->width,
            fb_info->height,
            rt.mouse_x,
            rt.mouse_y,
            cursor_kind
        );

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
